#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"

class IMaterialEditor;
class UMaterial;
class UMaterialGraph;
class UMaterialExpressionNamedRerouteDeclaration;
class UMaterialExpressionNamedRerouteUsage;
struct FParameterChannelNames;

/** Material graph helpers shared by the VRR material editor tools. */
namespace VrrGraph
{
	/** One place that reads a node: an expression input, or one of the material's own outputs (Consumer null). */
	struct FLink
	{
		UMaterialExpression* Consumer = nullptr;
		FExpressionInput* Input = nullptr;
	};

	using FReaders = TMap<UMaterialExpression*, TArray<FLink>>;

	/**
	 * Where the tools put reroute nodes, in graph units. Node sizes aren't known until the graph widgets are drawn,
	 * so these are estimates that read well for typical nodes.
	 */
	namespace Layout
	{
		constexpr int32 UsageOffset = 300;		// a usage sits this far left of the node it feeds
		constexpr int32 UsageStep = 48;			// usages stacked beside one node
		constexpr int32 PinTop = 36;			// first pin row below a node's top
		constexpr int32 PinStep = 24;			// pin row height
		constexpr int32 DeclarationStep = 48;	// declarations stacked beside one node
	}

	/** Channels a mask keeps (works for FExpressionInput and FExpressionOutput); 0 when there's no mask. */
	template <typename TMasked>
	int32 MaskWidth(const TMasked& M)
	{
		return M.Mask ? (M.MaskR != 0) + (M.MaskG != 0) + (M.MaskB != 0) + (M.MaskA != 0) : 0;
	}

	/** A usage for Read: beside the reading node (or the Result node), stacked by Slot. */
	FIntPoint UsagePositionFor(const UMaterial* Material, const UMaterialExpression* Consumer, int32 Slot);

	/** A usage beside pin Row of a node at (NodeX, NodeY). */
	FIntPoint UsagePositionForPin(int32 NodeX, int32 NodeY, int32 Row);

	/** Who reads each node: every expression input, and the material's own outputs (hidden legacy ones too). */
	FReaders MapReaders(UMaterial* Material);

	TSharedPtr<IMaterialEditor> GetEditor(const UMaterialGraph* Graph);

	/** Expressions in the material editor's selection that pass Filter. */
	TArray<UMaterialExpression*> GetSelectedExpressions(const UMaterialGraph* Graph, TFunctionRef<bool(UMaterialExpression*)> Filter);

	/** Top to bottom, then left to right, as nodes sit in the graph. */
	bool ByPosition(const UMaterialExpression& A, const UMaterialExpression& B);

	/** The channel (0-3 = R/G/B/A) a single-channel mask keeps, else INDEX_NONE. */
	template <typename TMasked>
	int32 SingleChannelOf(const TMasked& M)
	{
		return MaskWidth(M) != 1 ? INDEX_NONE : M.MaskR ? 0 : M.MaskG ? 1 : M.MaskB ? 2 : 3;
	}

	/** Custom channel names (Parameter Customization) of a vector parameter or texture sample; a plain Texture Sample
	 *  with none of its own uses its texture object parameter's. A Dynamic Parameter's are its Param Names (not the
	 *  default "ParamN"). */
	FParameterChannelNames ChannelNamesOf(const UMaterialExpression* E);

	/** Name for one channel (0-3) of E's value: its custom channel name, else "<Base> R/G/B/A". */
	FString ChannelName(const UMaterialExpression* E, const FString& Base, int32 Channel);

	/** New named reroute declaration reading Source's Pin. Name is made unique if taken. */
	UMaterialExpressionNamedRerouteDeclaration* CreateDeclaration(UMaterial* Material, UMaterialExpression* Source, int32 Pin,
		int32 X, int32 Y, const FString& Name, const FLinearColor& Color);

	/** New usage of Declaration. */
	UMaterialExpressionNamedRerouteUsage* CreateUsage(UMaterial* Material, UMaterialExpressionNamedRerouteDeclaration* Declaration, FIntPoint Position);

	/** Makes Usage read Declaration. */
	void BindUsage(UMaterialExpressionNamedRerouteUsage* Usage, UMaterialExpressionNamedRerouteDeclaration* Declaration);

	/** Points Read at Usage (whole value; its channel mask is the caller's call). */
	void Rewire(const FLink& Read, UMaterialExpression* Usage);

	/** After an edit: rebuild the graph, tell the editor the material changed, and select Select (replacing the selection). */
	void FinishEdit(UMaterialGraph* Graph, IMaterialEditor& Editor, TConstArrayView<const UMaterialExpression*> Select);

	/** Success / failure toast. */
	void Notify(const FText& Message, bool bSuccess);
}
