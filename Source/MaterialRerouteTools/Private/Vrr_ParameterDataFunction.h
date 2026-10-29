#pragma once

#include "CoreMinimal.h"

class UMaterialGraph;
class UMaterialExpression;
class UMaterialExpressionMaterialFunctionCall;

/**
 * "Create Parameter Data Function": moves selected material parameters (and the few nodes that only shape them) into
 * a new material function, and leaves named reroutes in the material.
 *
 *   Moved:     selected Scalar / Vector / Texture Sample parameters; selected Texture Object parameters together with
 *              the Texture Samples they feed; Append nodes whose inputs all come from moved nodes.
 *   Function:  copies of the moved nodes; one output per value the rest of the material reads, named
 *              "<Name> (S|V2|V3|V4)" with the type taken from how it's used; links from outside the moved nodes
 *              (e.g. UVs) become function inputs.
 *   Material:  the moved nodes are replaced by a call to the function; each output feeds a named reroute
 *              "s_|vec2_|vec3_|vec4_<Name>", and every place that read a moved node now reads a usage of it.
 *
 * Parameters keep their names, groups and defaults inside the function, so material instances keep their overrides.
 *
 * "Add to Parameter Data Function" does the same into a function the material already calls, appending outputs
 * (and inputs) after the existing ones and reusing what the function already has.
 */
namespace VrrParameterData
{
	/** Parameters in the material editor's current selection that this tool can move (other nodes are ignored). */
	TArray<UMaterialExpression*> GetSelectedParameters(const UMaterialGraph* Graph);

	/** Runs the whole operation on the selection: asks where to save the function, builds it, rewires the material. */
	void CreateFromSelection(UMaterialGraph* Graph);

	/** One call per parameter data function the material calls (named PARAM_*, or made by this tool), sorted by name. */
	TArray<UMaterialExpressionMaterialFunctionCall*> GetParameterDataCalls(const UMaterialGraph* Graph);

	/** Runs the operation into the function Call calls, wiring the new outputs through Call. */
	void AddToFunction(UMaterialGraph* Graph, UMaterialExpressionMaterialFunctionCall* Call);
}
