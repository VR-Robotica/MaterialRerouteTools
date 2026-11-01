// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "Vrr_UpdateReroutes.generated.h"

class UMaterialGraph;

/**
 * Bring a graph's existing named reroutes up to the project's style (Project Settings > Plugins > Material Reroute
 * Tools), e.g. after the team changes it. Both work on every declaration of the material or material function, as one
 * undo step; usages follow their declaration.
 *
 * "Update Named Reroute Colors": each declaration gets its type's color. The type comes from the name prefix (what
 * people read), else from what feeds it; reroutes with neither keep their color.
 *
 * "Update Named Reroute Prefixes...": each declaration's prefix is checked against the type it carries (what feeds
 * it; an unwired one goes by its prefix) and renamed if it's missing, out of date or for another type:
 *
 *   Normal          -> vec3_Normal   (no prefix)
 *   float3_Normal   -> vec3_Normal   (a default or Old Prefixes entry, see VrrReroute::MatchPrefix)
 *   s_Normal        -> vec3_Normal   (a prefix for another type)
 *
 * The renames are listed for confirmation first, and renamed reroutes also get their type's color.
 */
namespace VrrUpdateReroutes
{
	/** How many of a graph's declarations an update would change. */
	struct FCount
	{
		int32 Declarations = 0;
		int32 ToChange = 0;
	};

	FCount CountColors(const UMaterialGraph* Graph);
	FCount CountPrefixes(const UMaterialGraph* Graph);

	/** Menu tooltips for those counts. */
	FText ColorsTooltip(const FCount& Count);
	FText PrefixesTooltip(const FCount& Count);

	void UpdateColors(UMaterialGraph* Graph);

	/** Lists the renames, and on OK applies them. */
	void UpdatePrefixes(UMaterialGraph* Graph);
}

/**
 * Adds both updates to the menu that opens on empty graph space: the schema asks every graph node class's default
 * object for entries (like UVrrPrefixedRerouteMenuNode). Never placed in a graph.
 */
UCLASS(Transient)
class UVrrUpdateReroutesMenuNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	virtual bool CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const override;
	virtual void GetMenuEntries(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
};
