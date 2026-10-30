// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "Vrr_PrefixedReroute.generated.h"

/**
 * "Create Prefixed Named Reroute", in the menu that opens when a wire is dragged off a pin (see Vrr_RerouteStyle
 * for prefixes and colors):
 *  - off a node's output: a declaration fed by that pin, "<prefix>reroute" ("vec3_reroute", "s_reroute", ...),
 *    typed from the wire;
 *  - off an input of the Result node or a Make / Set Material Attributes node: a declaration "<prefix><input name>"
 *    plus a usage feeding the input; an existing wire into the input now goes through them.
 *
 * The drag-off menu is built by the graph schema, not ToolMenus. Before its own entries the schema asks the class
 * default object of every graph node class for menu entries, so this class exists only for that: it's never placed
 * in a graph.
 */
UCLASS(Transient)
class UVrrPrefixedRerouteMenuNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	virtual bool CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const override;
	virtual void GetMenuEntries(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
};
