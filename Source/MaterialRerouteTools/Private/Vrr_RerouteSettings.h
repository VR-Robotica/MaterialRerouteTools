// Copyright VR Robotica. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Vrr_RerouteSettings.generated.h"

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
 * existing reroutes keep their names and colors.
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Material Reroute Tools"))
class UVrrRerouteSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UVrrRerouteSettings& Get() { return *GetDefault<UVrrRerouteSettings>(); }

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Boolean"))
	FVrrRerouteStyle Bool = { TEXT("b_"), FLinearColor(1.0f, 0.0f, 0.0f) };	// red

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle Scalar = { TEXT("s_"), FLinearColor(0.0f, 1.0f, 0.0f) };	// green

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 2"))
	FVrrRerouteStyle Vector2 = { TEXT("vec2_"), FLinearColor(0.6f, 1.0f, 0.0f) };	// yellow green

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 3"))
	FVrrRerouteStyle Vector3 = { TEXT("vec3_"), FLinearColor(1.0f, 1.0f, 0.0f) };	// yellow

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Vector 4"))
	FVrrRerouteStyle Vector4 = { TEXT("vec4_"), FLinearColor(1.0f, 0.5f, 0.0f) };	// orange

	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Texture Object"))
	FVrrRerouteStyle Texture = { TEXT("tex_"), FLinearColor(0.0f, 0.0f, 1.0f) };	// blue

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle MaterialAttributes = { TEXT("ma_"), FLinearColor(0.5f, 0.0f, 1.0f) };	// purple

	UPROPERTY(config, EditAnywhere, Category = "Value Types")
	FVrrRerouteStyle ShadingModel = { TEXT("sm_"), FLinearColor(0.0f, 0.5f, 0.5f) };	// teal

	/** Type that couldn't be worked out: no prefix, this color. */
	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (DisplayName = "Unknown Type Color", HideAlphaChannel))
	FLinearColor UnknownColor = FLinearColor(0.5f, 0.5f, 0.5f);	// grey

	/** Color of a reroute reading the RGB of a normal-map sampler (its prefix still follows its type). */
	UPROPERTY(config, EditAnywhere, Category = "Value Types", meta = (HideAlphaChannel))
	FLinearColor NormalMapColor = FLinearColor(0.5f, 0.5f, 1.0f);
};
