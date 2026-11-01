// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Vrr_RerouteSettings.generated.h"

/** The plugin's own default prefixes. "Update Named Reroute Prefixes" also recognizes these after a project changes them. */
namespace VrrDefaultPrefix
{
	inline constexpr const TCHAR* Bool = TEXT("b_");
	inline constexpr const TCHAR* Scalar = TEXT("s_");
	inline constexpr const TCHAR* Vector2 = TEXT("vec2_");
	inline constexpr const TCHAR* Vector3 = TEXT("vec3_");
	inline constexpr const TCHAR* Vector4 = TEXT("vec4_");
	inline constexpr const TCHAR* Texture = TEXT("tex_");
	inline constexpr const TCHAR* MaterialAttributes = TEXT("ma_");
	inline constexpr const TCHAR* ShadingModel = TEXT("sm_");
}

/** Name prefix and node color of the named reroutes made for one value type. */
USTRUCT()
struct FVrrRerouteStyle
{
	GENERATED_BODY()

	FVrrRerouteStyle() = default;
	FVrrRerouteStyle(const TCHAR* InPrefix, const FLinearColor& InColor) : Prefix(InPrefix), Color(InColor) {}

	/** Put in front of the reroute's name ("s_" + "Roughness" -> "s_Roughness"). May be empty. */
	UPROPERTY(EditAnywhere, Category = "Style")
	FString Prefix;

	UPROPERTY(EditAnywhere, Category = "Style", meta = (HideAlphaChannel))
	FLinearColor Color = FLinearColor::White;
};

/**
 * Project Settings > Plugins > Material Reroute Tools: the prefix and color of each value type's named reroutes.
 * Saved to the project's DefaultEditor.ini so a team shares one style. Changes apply to reroutes made afterwards;
 * existing reroutes keep their names and colors until "Update Named Reroute Prefixes" / "Colors" run on their graph.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Material Reroute Tools"))
class UVrrRerouteSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UVrrRerouteSettings& Get() { return *GetDefault<UVrrRerouteSettings>(); }

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Boolean"))
	FVrrRerouteStyle Bool = { VrrDefaultPrefix::Bool, FLinearColor(1.0f, 0.0f, 0.0f) };	// red

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle Scalar = { VrrDefaultPrefix::Scalar, FLinearColor(0.0f, 1.0f, 0.0f) };	// green

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 2"))
	FVrrRerouteStyle Vector2 = { VrrDefaultPrefix::Vector2, FLinearColor(0.6f, 1.0f, 0.0f) };	// yellow green

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 3"))
	FVrrRerouteStyle Vector3 = { VrrDefaultPrefix::Vector3, FLinearColor(1.0f, 1.0f, 0.0f) };	// yellow

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 4"))
	FVrrRerouteStyle Vector4 = { VrrDefaultPrefix::Vector4, FLinearColor(1.0f, 0.5f, 0.0f) };	// orange

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Texture Object"))
	FVrrRerouteStyle Texture = { VrrDefaultPrefix::Texture, FLinearColor(0.0f, 0.0f, 1.0f) };	// blue

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle MaterialAttributes = { VrrDefaultPrefix::MaterialAttributes, FLinearColor(0.5f, 0.0f, 1.0f) };	// purple

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle ShadingModel = { VrrDefaultPrefix::ShadingModel, FLinearColor(0.0f, 0.5f, 0.5f) };	// teal

	/** Type that couldn't be worked out: no prefix, this color. */
	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Unknown Type Color", HideAlphaChannel))
	FLinearColor UnknownColor = FLinearColor(0.5f, 0.5f, 0.5f);	// grey

	/** Color of a reroute reading the RGB of a normal-map sampler (its prefix still follows its type). */
	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (HideAlphaChannel))
	FLinearColor NormalMapColor = FLinearColor(0.5f, 0.5f, 1.0f);

	/**
	 * Prefixes the project used before ("float3_", "v_", ...). "Update Named Reroute Prefixes" replaces them with the
	 * current prefix for the reroute's type. The plugin's default prefixes and the ones above are always recognized.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Renaming")
	TArray<FString> OldPrefixes;
};
