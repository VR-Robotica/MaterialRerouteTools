/*
 * Create Prefixed Named Reroute, from the menu that opens when a wire is dragged off a pin.
 *
 * Off a node's output: a declaration fed by that pin (every input the pin already fed now reads a usage of it placed
 * beside the input), named "<prefix><name>" where the name is the parameter, node or
 * pin it reads ("s_Roughness", "vec3_Camera Vector", "s_Light Mask"; see VrrReroute::NameOfOutput), else
 * "<prefix>reroute". The prefix comes from
 *  1. the type the material compiler stored on the pin, when there is one (the new material translator does this
 *     for wire colors), else
 *  2. the type worked out from the graph (VrrReroute::InferKind: the node's own type, or for generic math its
 *     widest input, through masks, appends, reroutes and function calls).
 *
 * Off any node's input: a declaration "<prefix><input name>" where the menu was opened and a usage beside the pin,
 * wired into the input. If the input was wired, the declaration takes over that wire (same source pin), so the value
 * is unchanged. Name and prefix follow the input's expected type (VrrOrganizeInputs::DescribeInputPin /
 * RerouteStyleFor): the material property for the Result node and Make / Set Material Attributes, the pin's type tag
 * for VRR nodes and function calls ("Base Color (V3)" -> "vec3_Base Color"), else what feeds it.
 *
 * Nodes are created and wired through the graph the way the engine's own "new node" action does it (one undo step),
 * so it works in materials and material functions alike (the Result node only exists in materials).
 */

#include "Vrr_PrefixedReroute.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_OrganizeInputs.h"
#include "Vrr_RerouteStyle.h"

#include "EdGraph/EdGraphSchema.h"
#include "MaterialEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialGraph/MaterialShaderValueTypeObject.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "ScopedTransaction.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(Vrr_PrefixedReroute)

#define LOCTEXT_NAMESPACE "VrrPrefixedReroute"

namespace
{
	/** The material node output a wire was dragged from, or null. */
	UMaterialGraphNode* SourceNodeOf(const UEdGraphPin* FromPin)
	{
		if (!FromPin || FromPin->Direction != EGPD_Output || FromPin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec)
		{
			return nullptr;
		}
		UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(FromPin->GetOwningNode());
		return Node && Node->MaterialExpression ? Node : nullptr;
	}

	/** Kind of the wire leaving FromPin: the compiled type stored on the pin, else worked out from the graph. */
	VrrReroute::EKind KindOfWire(const UEdGraphPin* FromPin, UMaterialExpression* Source)
	{
		if (const UMaterialShaderValueTypeObject* Compiled = Cast<UMaterialShaderValueTypeObject>(FromPin->PinType.PinSubCategoryObject.Get()))
		{
			const VrrReroute::EKind Kind = VrrReroute::KindOfShaderType(Compiled->ValueType);
			if (Kind != VrrReroute::EKind::Unknown)
			{
				return Kind;
			}
		}
		return VrrReroute::InferKind(Source, FromPin->SourceIndex);
	}

	/** "<prefix><output name>" when the output has a name, else "<prefix>reroute". */
	FString OutputRerouteName(const UMaterialExpression* Source, int32 Pin, VrrReroute::EKind Kind)
	{
		const FString OutputName = VrrReroute::NameOfOutput(Source, Pin);
		return FString::Printf(TEXT("%s%s"), VrrReroute::Prefix(Kind), OutputName.IsEmpty() ? TEXT("reroute") : *OutputName);
	}

	/** A new expression added through the graph (as the engine's "new node" action does), or null if it has no graph node. */
	template <typename TExpression>
	TExpression* NewGraphExpression(UEdGraph* Graph, const FVector2f& Location, bool bSelect)
	{
		TExpression* Expression = Cast<TExpression>(FMaterialEditorUtilities::CreateNewMaterialExpression(
			Graph, TExpression::StaticClass(), FDeprecateSlateVector2D(Location), bSelect, /*bAutoAssignResource*/ false));
		return Expression && Cast<UMaterialGraphNode>(Expression->GraphNode) ? Expression : nullptr;
	}

	UMaterialGraphNode* GraphNodeOf(const UMaterialExpression* E) { return CastChecked<UMaterialGraphNode>(E->GraphNode); }

	UMaterialExpressionNamedRerouteDeclaration* NewDeclaration(UEdGraph* Graph, const FVector2f& Location, bool bSelect, const FString& Name, const FLinearColor& Color)
	{
		UMaterialExpressionNamedRerouteDeclaration* Declaration = NewGraphExpression<UMaterialExpressionNamedRerouteDeclaration>(Graph, Location, bSelect);
		if (Declaration)
		{
			Declaration->NodeColor = Color;
			Declaration->SetEditableName(Name);	// made unique if taken
		}
		return Declaration;
	}

	/** Row of an input pin on its node, counting the visible value inputs above it (where a usage beside it goes). */
	int32 InputRowOf(const UEdGraphPin* Pin)
	{
		int32 Row = 0;
		for (const UEdGraphPin* Other : Pin->GetOwningNode()->Pins)
		{
			if (Other == Pin)
			{
				break;
			}
			Row += Other->Direction == EGPD_Input && !Other->bHidden && Other->PinType.PinCategory != UMaterialGraphSchema::PC_Exec;
		}
		return Row;
	}

	/** A usage of Declaration beside InputPin (pin Row of its node), made the input's only wire. */
	void FeedFromUsage(UEdGraph* Graph, UMaterialExpressionNamedRerouteDeclaration* Declaration, UEdGraphPin* InputPin, int32 Row)
	{
		const UEdGraphNode* Owner = InputPin->GetOwningNode();
		const FIntPoint UsagePosition = VrrGraph::UsagePositionForPin(Owner->NodePosX, Owner->NodePosY, Row);
		UMaterialExpressionNamedRerouteUsage* Usage = NewGraphExpression<UMaterialExpressionNamedRerouteUsage>(Graph, FVector2f(UsagePosition), false);
		if (!Usage)
		{
			return;
		}
		VrrGraph::BindUsage(Usage, Declaration);
		Usage->GraphNode->ReconstructNode();	// pick up the declaration's name and color
		const UEdGraphSchema* Schema = Graph->GetSchema();
		Schema->BreakPinLinks(*InputPin, true);
		if (UEdGraphPin* UsageOut = GraphNodeOf(Usage)->GetOutputPin(0))
		{
			Schema->TryCreateConnection(UsageOut, InputPin);
		}
	}

	/** Inputs FromPin feeds that a new declaration takes over (each then reads a usage). Other declarations keep their direct wire. */
	TArray<UEdGraphPin*> InputsToReroute(const UEdGraphPin* FromPin)
	{
		TArray<UEdGraphPin*> Inputs;
		for (UEdGraphPin* Linked : FromPin->LinkedTo)
		{
			const UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Linked->GetOwningNode());
			if (!Node || !Cast<UMaterialExpressionNamedRerouteDeclaration>(Node->MaterialExpression))
			{
				Inputs.Add(Linked);
			}
		}
		return Inputs;
	}

	/**
	 * Dragged off a node's output: a declaration fed by it. Every input the output already fed now reads a usage of the
	 * declaration beside it, so the long wires go away.
	 */
	struct FFromOutputAction : public FEdGraphSchemaAction
	{
		using FEdGraphSchemaAction::FEdGraphSchemaAction;
		using FEdGraphSchemaAction::PerformAction;

		virtual UEdGraphNode* PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode) override
		{
			UMaterialGraphNode* SourceNode = SourceNodeOf(FromPin);
			if (!SourceNode)
			{
				return nullptr;
			}
			const FScopedTransaction Transaction(LOCTEXT("Transaction", "Create Prefixed Named Reroute"));
			UMaterialExpression* Source = SourceNode->MaterialExpression;
			const VrrReroute::EKind Kind = KindOfWire(FromPin, Source);
			UMaterialExpressionNamedRerouteDeclaration* Declaration = NewDeclaration(ParentGraph, Location, bSelectNewNode,
				OutputRerouteName(Source, FromPin->SourceIndex, Kind), VrrReroute::ColorFor(Source, FromPin->SourceIndex, Kind));
			if (!Declaration)
			{
				return nullptr;
			}
			const TArray<UEdGraphPin*> Rerouted = InputsToReroute(FromPin);	// before the declaration's own wire is added
			Declaration->GraphNode->AutowireNewNode(FromPin);	// an output can feed many: just adds this wire
			for (UEdGraphPin* Input : Rerouted)
			{
				FeedFromUsage(ParentGraph, Declaration, Input, InputRowOf(Input));
			}
			if (!Rerouted.IsEmpty())
			{
				FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(ParentGraph);
			}
			return Declaration->GraphNode;
		}
	};

	/** Dragged off a named input: a declaration (taking over the input's wire, if any) and a usage into the input. */
	struct FFromInputAction : public FEdGraphSchemaAction
	{
		using FEdGraphSchemaAction::FEdGraphSchemaAction;
		using FEdGraphSchemaAction::PerformAction;

		virtual UEdGraphNode* PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode) override
		{
			const UMaterialGraph* Graph = Cast<UMaterialGraph>(ParentGraph);
			const TOptional<VrrOrganizeInputs::FNamedInput> Named = VrrOrganizeInputs::DescribeInputPin(Graph, FromPin);
			if (!Named)
			{
				return nullptr;
			}
			const FScopedTransaction Transaction(LOCTEXT("Transaction", "Create Prefixed Named Reroute"));
			const UEdGraphSchema* Schema = ParentGraph->GetSchema();
			FString Name;
			FLinearColor Color;
			VrrOrganizeInputs::RerouteStyleFor(*Named, Name, Color);

			// 1. The declaration where the menu was opened, fed by whatever fed the input (same source pin)
			UMaterialExpressionNamedRerouteDeclaration* Declaration = NewDeclaration(ParentGraph, Location, bSelectNewNode, Name, Color);
			if (!Declaration)
			{
				return nullptr;
			}
			UEdGraphPin* SourcePin = FromPin->LinkedTo.IsEmpty() ? nullptr : FromPin->LinkedTo[0];
			UEdGraphPin* DeclarationIn = GraphNodeOf(Declaration)->GetInputPin(0);
			if (SourcePin && DeclarationIn)
			{
				Schema->TryCreateConnection(SourcePin, DeclarationIn);	// from the same source pin, so the same channels
			}

			// 2. A usage beside the pin, now the input's only wire
			FeedFromUsage(ParentGraph, Declaration, FromPin, Named->Row);
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(ParentGraph);
			return Declaration->GraphNode;
		}
	};
}

bool UVrrPrefixedRerouteMenuNode::CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const
{
	return Schema && Schema->IsA<UMaterialGraphSchema>();
}

void UVrrPrefixedRerouteMenuNode::GetMenuEntries(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	const UMaterialGraph* Graph = Cast<UMaterialGraph>(ContextMenuBuilder.CurrentGraph);
	const UEdGraphPin* FromPin = ContextMenuBuilder.FromPin;
	if (!Graph || !FromPin)
	{
		return;
	}
	const FText Category = LOCTEXT("Category", "Named Reroutes");
	const FText Label = LOCTEXT("Label", "Create Prefixed Named Reroute");
	const FText Keywords = LOCTEXT("Keywords", "named reroute declaration prefix");
	constexpr int32 Grouping = 2;	// on top, like the engine's named reroutes

	if (UMaterialGraphNode* SourceNode = SourceNodeOf(FromPin))
	{
		UMaterialExpression* Source = SourceNode->MaterialExpression;
		const FString Name = OutputRerouteName(Source, FromPin->SourceIndex, KindOfWire(FromPin, Source));
		const int32 Rerouted = InputsToReroute(FromPin).Num();
		ContextMenuBuilder.AddAction(MakeShared<FFromOutputAction>(Category, Label, Rerouted
			? FText::Format(LOCTEXT("OutputTooltipRerouted", "Add a named reroute declaration \"{0}\" fed by this pin; the {1} {1}|plural(one=input,other=inputs) it feeds {1}|plural(one=reads,other=each read) a usage of it instead."),
				FText::FromString(Name), Rerouted)
			: FText::Format(LOCTEXT("OutputTooltip", "Add a named reroute declaration \"{0}\" fed by this pin, prefixed and colored for the wire's type."),
				FText::FromString(Name)), Grouping, Keywords));
	}
	else if (const TOptional<VrrOrganizeInputs::FNamedInput> Named = VrrOrganizeInputs::DescribeInputPin(Graph, FromPin))
	{
		FString Name;
		FLinearColor Color;
		VrrOrganizeInputs::RerouteStyleFor(*Named, Name, Color);
		ContextMenuBuilder.AddAction(MakeShared<FFromInputAction>(Category, Label,
			FText::Format(Named->Input->Expression
				? LOCTEXT("InputTooltipWired", "Route this input through a named reroute \"{0}\": the declaration takes over its wire, a usage feeds the input.")
				: LOCTEXT("InputTooltip", "Add a named reroute \"{0}\" for this input: a declaration here and a usage feeding the input."),
				FText::FromString(Name)), Grouping, Keywords));
	}
}

#undef LOCTEXT_NAMESPACE
