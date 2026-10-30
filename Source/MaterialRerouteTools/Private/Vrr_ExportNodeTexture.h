// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;
class UEdGraphPin;
class UMaterialGraph;

/**
 * Export Node to Texture: bakes what one output of a material node gives into a texture asset, across the texture's
 * 0-1 UVs (what the node shows when previewed on a flat plane).
 *
 * The material being edited is copied into a temporary Unlit material whose Emissive (or Substrate Unlit BSDF) is
 * that output, which is drawn into a float render target and saved as a texture. In a material function, a copy of
 * the function gets an extra output for the node and a temporary material calls it; function inputs the node reads
 * need "Use Preview Value as Default", since nothing feeds them. A 4-channel output takes a second
 * pass for its alpha. Sampling the texture gives the node's values back: Color stores them as sRGB, Linear as plain
 * 0-1 data, HDR as 16-bit floats (values outside 0-1 kept).
 *
 * Values that need a scene (world position, camera, scene textures, vertex data) have no meaning on a flat render
 * target, so nodes are best exported when they only depend on UVs, parameters and time.
 */
namespace VrrExportNode
{
	struct FOutput
	{
		int32 Index = 0;
		FText Label;	// the output pin's name, or the node's title when it has a single unnamed output
	};

	/** Outputs of Node that hold values (not textures, material attributes, shading models or Substrate). */
	TArray<FOutput> GetExportableOutputs(const UEdGraphNode* Node);

	/**
	 * In a material function: names of the function inputs Node reads from (directly or further upstream) that don't
	 * have "Use Preview Value as Default" on. Those have no value outside a call, so the node can't be exported.
	 */
	TArray<FName> FunctionInputsWithoutDefault(const UEdGraphNode* Node);

	/** Index of the output Pin is, or INDEX_NONE if it's an input or not on Node. */
	int32 OutputIndexOfPin(const UEdGraphNode* Node, const UEdGraphPin* Pin);

	/** Asks for size, format and where to save, then bakes Output of Node into a texture. */
	void Export(UMaterialGraph* Graph, const UEdGraphNode* Node, int32 Output);
}
