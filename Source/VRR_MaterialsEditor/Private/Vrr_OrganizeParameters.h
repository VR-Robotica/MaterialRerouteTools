#pragma once

#include "CoreMinimal.h"

class UMaterialGraph;
class UMaterialExpression;

/**
 * "Organize Parameters into Named Reroutes": gathers the selected parameters into a column at the top left of the
 * material graph, gives every value read from them a named reroute declaration beside them, and replaces every read
 * with a usage of that reroute next to the node that reads it. Nothing leaves the material, and parameter names,
 * groups and defaults stay as they are.
 */
namespace VrrOrganizeParameters
{
	/** Parameters in the current selection this tool organizes (other nodes are ignored). */
	TArray<UMaterialExpression*> GetSelectedParameters(const UMaterialGraph* Graph);

	/** Runs the whole operation on the selection, as one undo step. */
	void OrganizeSelection(UMaterialGraph* Graph);
}
