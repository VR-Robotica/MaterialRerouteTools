// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MaterialValueType.h"

class UEdGraphNode;
class UEdGraphPin;
class UMaterialGraph;
struct FExpressionInput;

/**
 * "Organize Inputs into Named Reroutes": for the material's Result node or a Make / Set Material Attributes node,
 * every wired input gets a named reroute declaration "<prefix><input name>" next to the node that feeds it, and the
 * input reads a usage of it placed beside the pin.
 *
 * The input naming and typing here is shared with Create Prefixed Named Reroute dragged off one of those inputs.
 */
namespace VrrOrganizeInputs
{
	/** One named input pin of a node. */
	struct FNamedInput
	{
		FString Name;							// as the node shows it, without a type tag ("Base Color")
		FExpressionInput* Input = nullptr;
		EMaterialValueType Type = MCT_Unknown;	// what the input expects (MCT_Unknown: work it out from what feeds it)
		int32 Row = 0;							// pin row on the node
	};

	/**
	 * The named input Pin stands for: an input of the Result node (materials only) or Make / Set Material Attributes,
	 * typed by material property, or any other node's input, named and typed as its pin shows it ("Base Color (V3)").
	 */
	TOptional<FNamedInput> DescribeInputPin(const UMaterialGraph* Graph, const UEdGraphPin* Pin);

	/** Name "<prefix><input name>" and color of a reroute carrying Input: by the type it expects, else what feeds it. */
	void RerouteStyleFor(const FNamedInput& Input, FString& OutName, FLinearColor& OutColor);

	/** True for the Result node and Make / Set Material Attributes nodes of a material (not a material function). */
	bool CanOrganize(const UMaterialGraph* Graph, const UEdGraphNode* Node);

	/** Wired inputs of Node that would get a reroute (not ones already reading a reroute usage). */
	int32 CountInputsToOrganize(UMaterialGraph* Graph, const UEdGraphNode* Node);

	/** Runs the operation on Node, as one undo step. */
	void OrganizeInputs(UMaterialGraph* Graph, const UEdGraphNode* Node);
}
