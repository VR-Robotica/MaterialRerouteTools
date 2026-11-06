// Copyright VR Robotica. All Rights Reserved.

#pragma once

/**
 * Engine API differences between UE 5.2 and the current engines (5.6+), so the rest of the plugin builds on both.
 * Versions in between are not built or tested.
 */

#include "CoreMinimal.h"
#include "MaterialValueType.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/MessageDialog.h"

#define VRR_UE_5_2 UE_VERSION_OLDER_THAN(5, 3, 0)

// Substrate was named Strata before 5.3
#if __has_include("Materials/MaterialExpressionSubstrate.h")
#include "Materials/MaterialExpressionSubstrate.h"
#else
#include "Materials/MaterialExpressionStrata.h"
#include "RenderUtils.h"
using UMaterialExpressionSubstrateBSDF = UMaterialExpressionStrataBSDF;
using UMaterialExpressionSubstrateUnlitBSDF = UMaterialExpressionStrataUnlitBSDF;
namespace Substrate
{
	inline bool IsSubstrateEnabled() { return Strata::IsStrataEnabled(); }
}
#define MCT_Substrate MCT_Strata
#endif

// Texture collections arrived in 5.5
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#define VRR_MCT_TEXTURE_COLLECTION 0
#else
#define VRR_MCT_TEXTURE_COLLECTION MCT_TextureCollection
#endif

// Sparse volume textures arrived in 5.3
#if VRR_UE_5_2
#define VRR_MCT_SPARSE_VOLUME_TEXTURE 0
#else
#define VRR_MCT_SPARSE_VOLUME_TEXTURE MCT_SparseVolumeTexture
#endif

// TArray::Pop takes EAllowShrinking since 5.4; 5.2 takes a bool
#if VRR_UE_5_2
#define VRR_NO_SHRINK false
#else
#define VRR_NO_SHRINK EAllowShrinking::No
#endif

// 5.2 doesn't export many engine input nodes (particle, position, ...) from the Engine module, so their StaticClass()
// can't be linked: find the class by name instead. Only for class objects (IsA, tables), not Cast<> to the type.
#if VRR_UE_5_2
#define VRR_EXPRESSION_CLASS(Name) FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpression" #Name))
#else
#define VRR_EXPRESSION_CLASS(Name) UMaterialExpression##Name::StaticClass()
#endif

// Graph actions place nodes at an FVector2f since 5.6; 5.2 passes an FVector2D by value
#if VRR_UE_5_2
#define VRR_GRAPH_LOCATION const FVector2D
#else
#define VRR_GRAPH_LOCATION const FVector2f&
#endif

#if VRR_UE_5_2
/** 5.2 has no FExpressionInputIterator: the same walk over an expression's inputs (Input, Index). */
struct FExpressionInputIterator
{
	explicit FExpressionInputIterator(UMaterialExpression* InExpression)
		: Inputs(InExpression ? InExpression->GetInputs() : TArray<FExpressionInput*>())
	{
		Input = Inputs.IsValidIndex(Index) ? Inputs[Index] : nullptr;
	}

	explicit operator bool() const { return Inputs.IsValidIndex(Index); }

	FExpressionInputIterator& operator++()
	{
		++Index;
		Input = Inputs.IsValidIndex(Index) ? Inputs[Index] : nullptr;
		return *this;
	}

	FExpressionInput* Input = nullptr;
	int32 Index = 0;

private:
	TArray<FExpressionInput*> Inputs;
};
#endif

namespace VrrCompat
{
	/** The value type an input expects. */
	inline EMaterialValueType InputValueType(UMaterialExpression* Expression, int32 InputIndex)
	{
#if VRR_UE_5_2
		return static_cast<EMaterialValueType>(Expression->GetInputType(InputIndex));
#else
		return Expression->GetInputValueType(InputIndex);
#endif
	}

	/** The value type an output produces. */
	inline EMaterialValueType OutputValueType(UMaterialExpression* Expression, int32 OutputIndex)
	{
#if VRR_UE_5_2
		return static_cast<EMaterialValueType>(Expression->GetOutputType(OutputIndex));
#else
		return Expression->GetOutputValueType(OutputIndex);
#endif
	}

	/** The material or material function a material graph edits. */
	inline UObject* MaterialOrFunctionOf(const UMaterialGraph* Graph)
	{
#if VRR_UE_5_2
		return Graph->Material ? static_cast<UObject*>(Graph->Material) : static_cast<UObject*>(Graph->MaterialFunction);
#else
		return Graph->GetMaterialOrFunction();
#endif
	}

	/** Modal message box with a title. */
	inline EAppReturnType::Type OpenDialog(EAppMsgType::Type Type, const FText& Message, const FText& Title)
	{
#if VRR_UE_5_2
		return FMessageDialog::Open(Type, Message, &Title);
#else
		return FMessageDialog::Open(Type, Message, Title);
#endif
	}
}
