#pragma once

#include "CoreMinimal.h"
#include "MaterialValueType.h"

class UMaterialExpression;
struct FExpressionInput;
namespace UE::Shader { enum class EValueType : uint8; }

/**
 * Shared naming and color rules for the named reroutes the VRR material tools create, so a value's type reads at a
 * glance in the graph:
 *
 *   Boolean b_ red, Scalar s_ green, Vector2 vec2_ yellow green, Vector3 vec3_ yellow, Vector4 vec4_ orange,
 *   Texture Object tex_ blue, Material Attributes ma_ purple, Shading Model sm_ teal, and the RGB pin of a
 *   normal-map sampler (0.5, 0.5, 1).
 */
namespace VrrReroute
{
	enum class EKind : uint8 { Unknown, Bool, Scalar, Vector2, Vector3, Vector4, Texture, MaterialAttributes, ShadingModel };

	/** What one output pin of a node says it gives (generic math says "scalar"; see InferKind). */
	EKind KindOf(UMaterialExpression* Source, int32 Pin);

	/** Kind of a value type (e.g. what a material input expects). Plain MCT_Float counts as a scalar. */
	EKind KindOfType(EMaterialValueType Type);

	/** Value type of a kind (the other way from KindOfType): Scalar -> MCT_Float1, Bool -> MCT_StaticBool, ... */
	EMaterialValueType TypeOfKind(EKind Kind);

	/** Kind of a compiled shader value type (what the new material translator stores on graph pins). */
	EKind KindOfShaderType(UE::Shader::EValueType Type);

	/**
	 * Kind of the value Source's Pin carries, worked out from the graph where the node itself only says "float"
	 * (Multiply, Lerp, ...): masks, appends, component masks, reroutes and function calls are followed, a few
	 * nodes are known (Dot, Length, Distance, Fresnel -> scalar, Cross -> vector3), and other math takes its widest
	 * wired input. A generic float with nothing wired is a scalar.
	 */
	EKind InferKind(UMaterialExpression* Source, int32 Pin);

	/** What a link carries: its channel mask if it has one, else InferKind of its source (Unknown if unwired). */
	EKind InferLinkKind(const FExpressionInput& Link);

	/**
	 * Splits a type tag off a pin name: "Light Mask (S)" -> "Light Mask" + Scalar. Tags: the engine's function input
	 * tags (S, V2, V3, V4, SB, B, T2d, TCube, T2dArr, TVol, TExt, MA) plus T and SM. A name without one comes back as
	 * is, with OutKind Unknown.
	 */
	FString SplitTypeTag(const FString& Name, EKind& OutKind);

	/**
	 * Name (without prefix) for a reroute reading Source's Pin, or empty if the pin has nothing worth naming it by:
	 *  - parameters (scalar, vector, static bool / switch, static component mask, texture, collection) and samplers
	 *    reading a texture object parameter: the parameter name; a single-channel pin its custom channel name, else
	 *    "<Name> R",
	 *  - engine input nodes, after the node: "UV0", "World Position" ("... XY", "... Z"), "Object Position",
	 *    "Camera Vector", "Vertex Color" ("... R"), ... (camera-relative and light-function variants as the captions),
	 *  - vertex interpolators: the name of what feeds VS (through reroutes; a named reroute's name minus its prefix),
	 *    else "Vertex Interpolator",
	 *  - function input nodes: the input name; other named pins (function calls, custom and VRR nodes): the pin name.
	 *    A type tag (" (S)") is dropped and channel labels ("RGB", "R", ...) don't count.
	 */
	FString NameOfOutput(const UMaterialExpression* Source, int32 Pin);

	/** Width (1-4) to kind; anything else is Unknown. */
	EKind KindForWidth(int32 Width);

	/** Named reroute name prefix: "b_", "s_", "vec2_", "vec3_", "vec4_", "tex_", "ma_", "sm_" (none for Unknown). */
	const TCHAR* Prefix(EKind Kind);

	/** Color per kind (Unknown gets a neutral grey). */
	FLinearColor Color(EKind Kind);

	/** Texture sample set to sample a normal map (a plain sampler also counts if its texture object does). */
	bool IsNormalSampler(const UMaterialExpression* E);

	/** Color for a reroute reading Source's Pin as Kind: the normal-map color for a normal sampler's RGB pin. */
	FLinearColor ColorFor(const UMaterialExpression* Source, int32 Pin, EKind Kind);
}
