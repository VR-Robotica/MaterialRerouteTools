// Copyright VR Robotica. All Rights Reserved.

/*
 * Organize Inputs into Named Reroutes: breaks the wires into a node's inputs into named reroutes that carry the
 * input's type prefix and name. Works on the material's Result node, Make / Set Material Attributes, Substrate nodes
 * (Slab, Unlit, Hair, ... BSDFs and the Substrate operators), and any other node whose visible inputs all have
 * descriptive names: function calls, VRR and engine C++ nodes (Vrr Cel Shadow, Custom, Texture Sample, Fresnel, ...).
 * Nodes with generic pins (math nodes' A / B, unnamed inputs) are left out, as "vec3_A" would say nothing.
 *
 * For every wired input this adds a declaration "<prefix><input name>", with the name exactly as the node shows it
 * ("vec3_Base Color", "s_Roughness", "vec3_Diffuse Albedo", "vec2_Customized UVs 0", ...), right next to the node
 * that feeds it. The input then reads a usage of it placed beside its pin. The value is unchanged: a channel mask on
 * the old link moves onto the declaration's input.
 *
 *  - Only visible pins count (Result node inputs the shading model / blend mode hides, hidden expression pins).
 *  - An input that already reads a named reroute usage is skipped, so a second run does nothing.
 *  - An input wired straight to a named reroute declaration just reads a usage of it, no new declaration.
 *  - Prefix and color follow the type the input expects (see Vrr_RerouteStyle): the material property for the Result
 *    node and the attribute nodes, else the pin's type tag ("Base Color (V3)" -> "vec3_Base Color", function calls
 *    and VRR nodes) or the type the node reports for it. A normal-map sampler's RGB gets the
 *    normal color. An input with no plain type takes what feeds it, else no prefix and grey. If the name is taken,
 *    the engine makes it unique.
 *
 * Steps
 *  1. Find the target node and list its inputs: name as shown, the input, the type it expects, its pin row.
 *  2. Per wired input: a declaration beside its source (or the declaration it already reads), then a usage beside
 *     the pin.
 *  3. Rebuild the graph, select the new declarations and report.
 */

#include "Vrr_OrganizeInputs.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "IMaterialEditor.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "VrrOrganizeInputs"

namespace VrrOrganizeInputs
{
	namespace
	{
		// Layout beyond VrrGraph::Layout, in graph units
		constexpr int32 SourceWidth = 300;		// declaration sits this far right of the node feeding it
		constexpr int32 UsageMinStep = 32;		// usages beside one node never overlap

		/** The node being organized: the Result node (Expression null) or an expression node with typed inputs. */
		struct FTarget
		{
			UMaterialExpression* Expression = nullptr;
			int32 X = 0;
			int32 Y = 0;
			TArray<FNamedInput> Inputs;
		};

		UMaterialExpression* ExpressionOf(const UEdGraphNode* Node)
		{
			const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
			return MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
		}

		bool IsAttributesNode(const UMaterialExpression* E)
		{
			return E && (E->IsA<UMaterialExpressionMakeMaterialAttributes>() || E->IsA<UMaterialExpressionSetMaterialAttributes>());
		}

		/** Substrate BSDFs (Slab, Unlit, Hair, ...) and operators: the node names and types its own inputs. */
		bool IsSubstrateNode(const UMaterialExpression* E)
		{
			return E && E->IsA<UMaterialExpressionSubstrateBSDF>();
		}

		/** How the report names the target node ("the Result node", "Make Material Attributes", "Substrate Slab BSDF"). */
		FText DisplayNameOf(const FTarget& Target)
		{
			UMaterialExpression* E = Target.Expression;
			if (!E)
			{
				return LOCTEXT("ResultNode", "the Result node");
			}
			if (IsAttributesNode(E))
			{
				return E->IsA<UMaterialExpressionMakeMaterialAttributes>()
					? LOCTEXT("MakeAttributes", "Make Material Attributes")
					: LOCTEXT("SetAttributes", "Set Material Attributes");
			}
			TArray<FString> Captions;
			E->GetCaption(Captions);
			return Captions.Num() > 0 ? FText::FromString(Captions[0]) : E->GetClass()->GetDisplayNameText();
		}

		/**
		 * Material property an input of Make stands for. Make reports a plain float for every input but Shading
		 * Model, so its real type has to come from the property. Same mapping as the node's own Compile (its
		 * GetExpressionInput isn't exported).
		 */
		TOptional<EMaterialProperty> PropertyOf(UMaterialExpressionMakeMaterialAttributes* Make, const FExpressionInput* Input)
		{
			using FMake = UMaterialExpressionMakeMaterialAttributes;
			static const TPair<FExpressionInput FMake::*, EMaterialProperty> Members[] = {
				{ &FMake::BaseColor, MP_BaseColor },
				{ &FMake::Metallic, MP_Metallic },
				{ &FMake::Specular, MP_Specular },
				{ &FMake::Roughness, MP_Roughness },
				{ &FMake::Anisotropy, MP_Anisotropy },
				{ &FMake::EmissiveColor, MP_EmissiveColor },
				{ &FMake::Opacity, MP_Opacity },
				{ &FMake::OpacityMask, MP_OpacityMask },
				{ &FMake::Normal, MP_Normal },
				{ &FMake::Tangent, MP_Tangent },
				{ &FMake::WorldPositionOffset, MP_WorldPositionOffset },
				{ &FMake::Displacement, MP_Displacement },
				{ &FMake::SubsurfaceColor, MP_SubsurfaceColor },
				{ &FMake::ClearCoat, MP_CustomData0 },
				{ &FMake::ClearCoatRoughness, MP_CustomData1 },
				{ &FMake::AmbientOcclusion, MP_AmbientOcclusion },
				{ &FMake::Refraction, MP_Refraction },
				{ &FMake::PixelDepthOffset, MP_PixelDepthOffset },
				{ &FMake::ShadingModel, MP_ShadingModel },
			};
			for (const TPair<FExpressionInput FMake::*, EMaterialProperty>& Member : Members)
			{
				if (&(Make->*Member.Key) == Input)
				{
					return Member.Value;
				}
			}
			for (int32 UV = 0; UV < UE_ARRAY_COUNT(Make->CustomizedUVs); ++UV)
			{
				if (&Make->CustomizedUVs[UV] == Input)
				{
					return static_cast<EMaterialProperty>(MP_CustomizedUVs0 + UV);
				}
			}
			return {};
		}

		/**
		 * Any expression node's input: the name as the pin shows it, its type tag split off ("Base Color (V3)", the
		 * VRR nodes and function calls) and giving the type, else the type the node reports. A plain "float" (math
		 * nodes take any width) counts as unknown, so the reroute follows what feeds it.
		 */
		TOptional<FNamedInput> DescribeExpressionInput(const UEdGraphNode* Node, int32 InputIndex)
		{
			UMaterialExpression* E = ExpressionOf(Node);
			FExpressionInput* Input = E ? E->GetInput(InputIndex) : nullptr;
			if (!Input)
			{
				return {};
			}
			// Inside a material function, an output node's pin is unnamed: the node carries the name ("Emissive (V3)")
			const UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(E);
			const FName Raw = FunctionOutput ? FunctionOutput->OutputName : E->GetInputName(InputIndex);
			VrrReroute::EKind Tagged = VrrReroute::EKind::Unknown;
			FString Name = VrrReroute::SplitTypeTag(Raw.ToString(), Tagged);
			if (!Name.Contains(TEXT(" ")))
			{
				Name = FName::NameToDisplayString(Name, false);	// property names: "BaseColor" -> "Base Color"
			}
			EMaterialValueType Type = Tagged != VrrReroute::EKind::Unknown ? VrrReroute::TypeOfKind(Tagged) : E->GetInputValueType(InputIndex);
			if (Type == MCT_Float)
			{
				Type = MCT_Unknown;
			}
			return FNamedInput{ Name, Input, Type, InputIndex, InputIndex };
		}

		/** Node's visible value inputs, in pin order (what the node shows, so also the usage rows). */
		TArray<FNamedInput> VisibleInputsOf(const UEdGraphNode* Node)
		{
			TArray<FNamedInput> Inputs;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->bHidden || Pin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec)
				{
					continue;
				}
				if (TOptional<FNamedInput> Input = DescribeExpressionInput(Node, Pin->SourceIndex))
				{
					Input->Row = Inputs.Num();
					Inputs.Add(MoveTemp(*Input));
				}
			}
			return Inputs;
		}

		/** A pin name that says nothing on its own: unnamed or a single letter ("A", "B", "X"). */
		bool IsGenericName(const FString& Name)
		{
			const FString Trimmed = Name.TrimStartAndEnd();
			return Trimmed.Len() <= 1 || Trimmed.Equals(TEXT("None"));
		}

		/**
		 * Any other C++ node (engine, VRR, third-party) or function call, when every visible input has a descriptive name:
		 * Custom, Texture Sample, Fresnel, Panner, Vrr Cel Shadow, MF_ToonLit, ... Math nodes (A / B, unnamed inputs)
		 * are left out, since "vec3_A" says nothing. Named reroute nodes are what this tool makes, so they never count.
		 */
		bool HasNamedInputs(const UEdGraphNode* Node, const UMaterialExpression* E)
		{
			if (E->IsA<UMaterialExpressionNamedRerouteBase>())
			{
				return false;
			}
			const TArray<FNamedInput> Inputs = VisibleInputsOf(Node);
			return Inputs.Num() > 0 && !Inputs.ContainsByPredicate([](const FNamedInput& I) { return IsGenericName(I.Name); });
		}

		/** The expression of a node whose inputs can be organized (attribute, Substrate or named-input nodes), else null. */
		UMaterialExpression* AsOrganizableExpression(const UEdGraphNode* Node)
		{
			UMaterialExpression* E = ExpressionOf(Node);
			return E && (IsAttributesNode(E) || IsSubstrateNode(E) || HasNamedInputs(Node, E)) ? E : nullptr;
		}

		/**
		 * 1a. An expression node's inputs. Attribute inputs are typed by the material property they set (Make reports a
		 * plain float for most of them); other nodes keep what their pins report.
		 */
		void ListExpressionInputs(const UEdGraphNode* Node, UMaterialExpression* E, FTarget& Out)
		{
			Out.Inputs = VisibleInputsOf(Node);
			if (!IsAttributesNode(E))
			{
				return;
			}
			UMaterialExpressionMakeMaterialAttributes* Make = Cast<UMaterialExpressionMakeMaterialAttributes>(E);
			for (FNamedInput& Input : Out.Inputs)
			{
				const TOptional<EMaterialProperty> Property = Make ? PropertyOf(Make, Input.Input) : TOptional<EMaterialProperty>();
				Input.Type = Property ? FMaterialAttributeDefinitionMap::GetValueType(*Property) : E->GetInputValueType(Input.Index);
			}
		}

		/** 1b. The Result node's visible inputs, typed by material property. */
		void ListResultInputs(const UMaterialGraph* Graph, UMaterial* Material, FTarget& Out)
		{
			int32 Row = 0;
			for (const FMaterialInputInfo& Info : Graph->MaterialInputs)
			{
				if (Info.IsVisiblePin(Material))
				{
					Out.Inputs.Add({ Info.GetName().ToString(), &Info.GetExpressionInput(Material),
						FMaterialAttributeDefinitionMap::GetValueType(Info.GetProperty()), Row++ });
				}
			}
		}

		/** 1. The node to organize and its inputs (Graph must be a material for the Result node). */
		bool FindTarget(const UMaterialGraph* Graph, const UEdGraphNode* Node, FTarget& Out)
		{
			if (UMaterialExpression* E = AsOrganizableExpression(Node))
			{
				Out.Expression = E;
				Out.X = E->MaterialExpressionEditorX;
				Out.Y = E->MaterialExpressionEditorY;
				ListExpressionInputs(Node, E, Out);
				return true;
			}
			UMaterial* Material = Graph ? Graph->Material.Get() : nullptr;
			if (!Node || !Node->IsA<UMaterialGraphNode_Root>() || !Material || Graph->MaterialFunction)
			{
				return false;
			}
			Out.X = Material->EditorX;
			Out.Y = Material->EditorY;
			ListResultInputs(Graph, Material, Out);
			return true;
		}

		bool NeedsReroute(const FNamedInput& Input)
		{
			return Input.Input->Expression && !Input.Input->Expression->IsA<UMaterialExpressionNamedRerouteUsage>();
		}

		/** What one run has done so far, and where the last usage went. */
		struct FRunState
		{
			TMap<UMaterialExpression*, int32> DeclarationsPerSource;	// stacks declarations beside a shared source
			TArray<const UMaterialExpression*> NewDeclarations;
			int32 Reused = 0;
			int32 PreviousUsageY = MIN_int32;
		};

		/**
		 * 2a. The declaration Entry will read: the one it's wired straight to (when there's no mask), else a new one
		 * beside the node feeding it, carrying the old link's channel mask.
		 */
		UMaterialExpressionNamedRerouteDeclaration* DeclarationFor(UMaterial* Material, const FNamedInput& Entry, FRunState& Run)
		{
			const FExpressionInput& Input = *Entry.Input;
			UMaterialExpression* Source = Input.Expression;
			UMaterialExpressionNamedRerouteDeclaration* Existing = Cast<UMaterialExpressionNamedRerouteDeclaration>(Source);
			if (Existing && !Input.Mask)
			{
				++Run.Reused;
				return Existing;
			}
			FString Name;
			FLinearColor Color;
			RerouteStyleFor(Entry, Name, Color);
			const int32 Slot = Run.DeclarationsPerSource.FindOrAdd(Source)++;
			UMaterialExpressionNamedRerouteDeclaration* Declaration = VrrGraph::CreateDeclaration(Material, Source, Input.OutputIndex,
				Source->MaterialExpressionEditorX + SourceWidth, Source->MaterialExpressionEditorY + Slot * VrrGraph::Layout::DeclarationStep, Name, Color);
			if (Declaration)
			{
				Declaration->Input.SetMask(Input.Mask, Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA);
				Run.NewDeclarations.Add(Declaration);
			}
			return Declaration;
		}

		/** 2b. The pin reads a usage of Declaration placed beside it, taking the whole value (the mask moved onto the declaration). */
		void ReadThroughUsage(UMaterial* Material, const FTarget& Target, const FNamedInput& Entry,
			UMaterialExpressionNamedRerouteDeclaration* Declaration, FRunState& Run)
		{
			FIntPoint Position = VrrGraph::UsagePositionForPin(Target.X, Target.Y, Entry.Row);
			Position.Y = FMath::Max(Position.Y, Run.PreviousUsageY + UsageMinStep);
			Run.PreviousUsageY = Position.Y;
			if (UMaterialExpressionNamedRerouteUsage* Usage = VrrGraph::CreateUsage(Material, Declaration, Position))
			{
				VrrGraph::Rewire({ Target.Expression, Entry.Input }, Usage);
				Entry.Input->SetMask(0, 0, 0, 0, 0);
			}
		}
	}

	TOptional<FNamedInput> DescribeInputPin(const UMaterialGraph* Graph, const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec)
		{
			return {};
		}
		const UEdGraphNode* Node = Pin->GetOwningNode();
		FTarget Target;
		if (!FindTarget(Graph, Node, Target))
		{
			return DescribeExpressionInput(Node, Pin->SourceIndex);
		}
		// Expression pins count inputs; the Result node's count Graph->MaterialInputs
		const FExpressionInput* Input = nullptr;
		if (Target.Expression)
		{
			Input = Target.Expression->GetInput(Pin->SourceIndex);
		}
		else if (Graph->MaterialInputs.IsValidIndex(Pin->SourceIndex))
		{
			Input = &Graph->MaterialInputs[Pin->SourceIndex].GetExpressionInput(Graph->Material);
		}
		const FNamedInput* Found = Input ? Target.Inputs.FindByPredicate([Input](const FNamedInput& I) { return I.Input == Input; }) : nullptr;
		return Found ? TOptional<FNamedInput>(*Found) : TOptional<FNamedInput>();
	}

	void RerouteStyleFor(const FNamedInput& Input, FString& OutName, FLinearColor& OutColor)
	{
		VrrReroute::EKind Kind = VrrReroute::KindOfType(Input.Type);
		if (Kind == VrrReroute::EKind::Unknown)
		{
			Kind = VrrReroute::InferLinkKind(*Input.Input);
		}
		OutName = FString::Printf(TEXT("%s%s"), VrrReroute::Prefix(Kind), *Input.Name);
		OutColor = VrrReroute::ColorForLink(*Input.Input, Kind);
	}

	bool CanOrganize(const UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		return Graph && Graph->Material && !Graph->MaterialFunction && Node
			&& (Node->IsA<UMaterialGraphNode_Root>() || AsOrganizableExpression(Node));
	}

	int32 CountInputsToOrganize(UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		FTarget Target;
		return CanOrganize(Graph, Node) && FindTarget(Graph, Node, Target) ? Target.Inputs.FilterByPredicate(NeedsReroute).Num() : 0;
	}

	void OrganizeInputs(UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		const TSharedPtr<IMaterialEditor> Editor = VrrGraph::GetEditor(Graph);
		FTarget Target;
		if (!Editor.IsValid() || !CanOrganize(Graph, Node) || !FindTarget(Graph, Node, Target))
		{
			return;
		}
		UMaterial* Material = Graph->Material;
		const FText Tool = LOCTEXT("Tool", "Organize Inputs into Named Reroutes");
		const FText NodeName = DisplayNameOf(Target);
		if (!Target.Inputs.ContainsByPredicate(NeedsReroute))
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("NothingToDo", "{0}: no inputs of {1} need a named reroute."), Tool, NodeName), false);
			return;
		}

		const FScopedTransaction Transaction(Tool);
		Material->Modify();
		if (Target.Expression)
		{
			Target.Expression->Modify();
		}

		// 2. Per wired input: declaration beside the source, usage beside the pin
		FRunState Run;
		int32 Skipped = 0;
		for (const FNamedInput& Entry : Target.Inputs)
		{
			if (!Entry.Input->Expression)
			{
				continue;
			}
			if (!NeedsReroute(Entry))
			{
				++Skipped;
				continue;
			}
			if (UMaterialExpressionNamedRerouteDeclaration* Declaration = DeclarationFor(Material, Entry, Run))
			{
				ReadThroughUsage(Material, Target, Entry, Declaration, Run);
			}
		}

		// 3. Show it, select the new declarations and report
		VrrGraph::FinishEdit(Graph, *Editor, Run.NewDeclarations);
		FText Message = FText::Format(LOCTEXT("Done", "Organized the inputs of {0}: {1} new named reroute(s), {2} existing reused."),
			NodeName, Run.NewDeclarations.Num(), Run.Reused);
		if (Skipped > 0)
		{
			Message = FText::Format(LOCTEXT("DoneSkipped", "{0}\n{1} input(s) already read a named reroute."), Message, Skipped);
		}
		VrrGraph::Notify(Message, true);
	}
}

#undef LOCTEXT_NAMESPACE
