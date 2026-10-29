/*
 * Organize Inputs into Named Reroutes: breaks the wires into the material's Result node (or a Make / Set Material
 * Attributes node) into named reroutes that carry the input's type prefix and name.
 *
 * For every wired input this adds a declaration "<prefix><input name>", with the name exactly as the node shows it
 * ("vec3_Base Color", "s_Roughness", "vec2_Customized UVs 0", ...), right next to the node that feeds it. The input
 * then reads a usage of it placed beside its pin. The value is unchanged: a channel mask on the old link moves onto
 * the declaration's input.
 *
 *  - Only the Result node's visible pins count (inputs the shading model / blend mode hides are left alone).
 *  - An input that already reads a named reroute usage is skipped, so a second run does nothing.
 *  - An input wired straight to a named reroute declaration just reads a usage of it, no new declaration.
 *  - Prefix and color follow the type the input expects (see Vrr_RerouteStyle); a normal-map sampler's RGB gets the
 *    normal color. An input with no plain type takes what feeds it, else no prefix and grey. If the name is taken,
 *    the engine makes it unique.
 *
 * Steps
 *  1. List the node's inputs: name as shown, the input, the type it expects, its pin row.
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
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "VrrOrganizeInputs"

namespace VrrOrganizeInputs
{
	namespace
	{
		// Layout beyond VrrGraph::Layout, in graph units
		constexpr int32 SourceWidth = 300;		// declaration sits this far right of the node feeding it
		constexpr int32 UsageMinStep = 32;		// usages beside one node never overlap

		/** The node being organized: the Result node (Expression null) or a Make / Set Material Attributes node. */
		struct FTarget
		{
			UMaterialExpression* Expression = nullptr;
			int32 X = 0;
			int32 Y = 0;
			TArray<FNamedInput> Inputs;
		};

		/** A Make or Set Material Attributes node's expression, or null. */
		UMaterialExpression* AsAttributesNode(const UEdGraphNode* Node)
		{
			const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
			UMaterialExpression* E = MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
			return E && (E->IsA<UMaterialExpressionMakeMaterialAttributes>() || E->IsA<UMaterialExpressionSetMaterialAttributes>()) ? E : nullptr;
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

		/** 1. The node's inputs, in pin order (Graph is a material for the Result node). */
		bool ListInputs(const UMaterialGraph* Graph, const UEdGraphNode* Node, FTarget& Out)
		{
			if (UMaterialExpression* Attributes = AsAttributesNode(Node))
			{
				Out.Expression = Attributes;
				Out.X = Attributes->MaterialExpressionEditorX;
				Out.Y = Attributes->MaterialExpressionEditorY;
				UMaterialExpressionMakeMaterialAttributes* Make = Cast<UMaterialExpressionMakeMaterialAttributes>(Attributes);
				for (FExpressionInputIterator It{ Attributes }; It; ++It)
				{
					// Pins show the input's name as display text ("BaseColor" -> "Base Color"); Set's are already
					const FString Name = FName::NameToDisplayString(Attributes->GetInputName(It.Index).ToString(), false);
					const TOptional<EMaterialProperty> Property = Make ? PropertyOf(Make, It.Input) : TOptional<EMaterialProperty>();
					const EMaterialValueType Type = Property ? FMaterialAttributeDefinitionMap::GetValueType(*Property) : Attributes->GetInputValueType(It.Index);
					Out.Inputs.Add({ Name, It.Input, Type, It.Index });
				}
				return true;
			}
			UMaterial* Material = Graph ? Graph->Material.Get() : nullptr;
			if (!Node || !Node->IsA<UMaterialGraphNode_Root>() || !Material || Graph->MaterialFunction)
			{
				return false;
			}
			Out.X = Material->EditorX;
			Out.Y = Material->EditorY;
			int32 Row = 0;
			for (const FMaterialInputInfo& Info : Graph->MaterialInputs)
			{
				if (Info.IsVisiblePin(Material))
				{
					Out.Inputs.Add({ Info.GetName().ToString(), &Info.GetExpressionInput(Material),
						FMaterialAttributeDefinitionMap::GetValueType(Info.GetProperty()), Row++ });
				}
			}
			return true;
		}

		/**
		 * Any other node's input pin: the name as the pin shows it, its type tag split off ("Base Color (V3)", the VRR
		 * nodes and function calls) and giving the type, else the type the node reports. A plain "float" (math nodes
		 * take any width) counts as unknown, so the reroute follows what feeds it.
		 */
		TOptional<FNamedInput> DescribeExpressionInput(const UEdGraphNode* Node, int32 InputIndex)
		{
			const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
			UMaterialExpression* E = MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
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
			return FNamedInput{ Name, Input, Type, InputIndex };
		}

		bool NeedsReroute(const FNamedInput& Input)
		{
			return Input.Input->Expression && !Input.Input->Expression->IsA<UMaterialExpressionNamedRerouteUsage>();
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
		if (!ListInputs(Graph, Node, Target))
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
		UMaterialExpression* Source = Input.Input->Expression;
		const int32 Pin = Input.Input->OutputIndex;
		VrrReroute::EKind Kind = VrrReroute::KindOfType(Input.Type);
		if (Kind == VrrReroute::EKind::Unknown)
		{
			Kind = VrrReroute::InferLinkKind(*Input.Input);
		}
		OutName = FString::Printf(TEXT("%s%s"), VrrReroute::Prefix(Kind), *Input.Name);
		OutColor = Source ? VrrReroute::ColorFor(Source, Pin, Kind) : VrrReroute::Color(Kind);
	}

	bool CanOrganize(const UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		return Graph && Graph->Material && !Graph->MaterialFunction && Node
			&& (Node->IsA<UMaterialGraphNode_Root>() || AsAttributesNode(Node));
	}

	int32 CountInputsToOrganize(UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		FTarget Target;
		return CanOrganize(Graph, Node) && ListInputs(Graph, Node, Target) ? Target.Inputs.FilterByPredicate(NeedsReroute).Num() : 0;
	}

	void OrganizeInputs(UMaterialGraph* Graph, const UEdGraphNode* Node)
	{
		const TSharedPtr<IMaterialEditor> Editor = VrrGraph::GetEditor(Graph);
		FTarget Target;
		if (!Editor.IsValid() || !CanOrganize(Graph, Node) || !ListInputs(Graph, Node, Target))
		{
			return;
		}
		UMaterial* Material = Graph->Material;
		const FText Tool = LOCTEXT("Tool", "Organize Inputs into Named Reroutes");
		const FText NodeName = Target.Expression
			? FText::FromString(Target.Expression->IsA<UMaterialExpressionMakeMaterialAttributes>() ? TEXT("Make Material Attributes") : TEXT("Set Material Attributes"))
			: LOCTEXT("ResultNode", "the Result node");
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
		TMap<UMaterialExpression*, int32> DeclarationsPerSource;
		TArray<const UMaterialExpression*> NewDeclarations;
		int32 Reused = 0;
		int32 Skipped = 0;
		int32 PreviousUsageY = MIN_int32;
		for (const FNamedInput& Entry : Target.Inputs)
		{
			FExpressionInput& Input = *Entry.Input;
			if (!Input.Expression)
			{
				continue;
			}
			if (!NeedsReroute(Entry))
			{
				++Skipped;
				continue;
			}
			UMaterialExpression* Source = Input.Expression;

			// 2a. Wired straight to a declaration: read a usage of it instead of stacking a second one
			UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Source);
			if (Declaration && !Input.Mask)
			{
				++Reused;
			}
			else
			{
				FString Name;
				FLinearColor Color;
				RerouteStyleFor(Entry, Name, Color);
				const int32 Slot = DeclarationsPerSource.FindOrAdd(Source)++;
				Declaration = VrrGraph::CreateDeclaration(Material, Source, Input.OutputIndex,
					Source->MaterialExpressionEditorX + SourceWidth, Source->MaterialExpressionEditorY + Slot * VrrGraph::Layout::DeclarationStep, Name, Color);
				if (!Declaration)
				{
					continue;
				}
				Declaration->Input.SetMask(Input.Mask, Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA);
				NewDeclarations.Add(Declaration);
			}

			// 2b. Usage beside the pin, reading the whole value (the mask now sits on the declaration)
			FIntPoint UsagePosition = VrrGraph::UsagePositionForPin(Target.X, Target.Y, Entry.Row);
			UsagePosition.Y = FMath::Max(UsagePosition.Y, PreviousUsageY + UsageMinStep);
			PreviousUsageY = UsagePosition.Y;
			if (UMaterialExpressionNamedRerouteUsage* Usage = VrrGraph::CreateUsage(Material, Declaration, UsagePosition))
			{
				VrrGraph::Rewire({ Target.Expression, &Input }, Usage);
				Input.SetMask(0, 0, 0, 0, 0);
			}
		}

		// 3. Show it, select the new declarations and report
		VrrGraph::FinishEdit(Graph, *Editor, NewDeclarations);
		FText Message = FText::Format(LOCTEXT("Done", "Organized the inputs of {0}: {1} new named reroute(s), {2} existing reused."),
			NodeName, NewDeclarations.Num(), Reused);
		if (Skipped > 0)
		{
			Message = FText::Format(LOCTEXT("DoneSkipped", "{0}\n{1} input(s) already read a named reroute."), Message, Skipped);
		}
		VrrGraph::Notify(Message, true);
	}
}

#undef LOCTEXT_NAMESPACE
