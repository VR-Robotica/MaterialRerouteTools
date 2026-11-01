// Copyright VR Robotica. All Rights Reserved.

/*
 * Shared rules for the named reroutes the VRR material tools create (see Vrr_RerouteStyle.h):
 *  - kinds: what a pin or wire carries (KindOf / InferKind: node types, type tags, masks, a table of engine input
 *    nodes that only say "float", and for generic math the widest input),
 *  - names: what a reroute off a pin is called (NameOfOutput: parameter, engine input node, function input, pin),
 *  - prefixes and colors per kind.
 */

#include "Vrr_RerouteStyle.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteSettings.h"

#include "Algo/AllOf.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionActorPositionWS.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLength.h"
#include "Materials/MaterialExpressionLightVector.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionObjectBounds.h"
#include "Materials/MaterialExpressionObjectOrientation.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionObjectRadius.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionParticleDirection.h"
#include "Materials/MaterialExpressionParticleMacroUV.h"
#include "Materials/MaterialExpressionParticleMotionBlurFade.h"
#include "Materials/MaterialExpressionParticlePositionWS.h"
#include "Materials/MaterialExpressionParticleRadius.h"
#include "Materials/MaterialExpressionParticleRandom.h"
#include "Materials/MaterialExpressionParticleRelativeTime.h"
#include "Materials/MaterialExpressionParticleSize.h"
#include "Materials/MaterialExpressionParticleSpeed.h"
#include "Materials/MaterialExpressionParticleSpriteRotation.h"
#include "Materials/MaterialExpressionParticleSubUV.h"
#include "Materials/MaterialExpressionParticleSubUVProperties.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionPreSkinnedNormal.h"
#include "Materials/MaterialExpressionPreSkinnedPosition.h"
#include "Materials/MaterialExpressionReflectionVectorWS.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionSphericalParticleOpacity.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticComponentMaskParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTwoSidedSign.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionVertexTangentWS.h"
#include "Materials/MaterialExpressionViewSize.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Shader/ShaderTypes.h"

namespace VrrReroute
{
	EKind KindForWidth(int32 Width)
	{
		switch (Width)
		{
		case 1:  return EKind::Scalar;
		case 2:  return EKind::Vector2;
		case 3:  return EKind::Vector3;
		case 4:  return EKind::Vector4;
		default: return EKind::Unknown;
		}
	}

	namespace
	{
		/** Channel count of a plain value kind (the other way from KindForWidth), 0 for anything else. */
		int32 WidthOf(EKind Kind)
		{
			switch (Kind)
			{
			case EKind::Scalar:  return 1;
			case EKind::Vector2: return 2;
			case EKind::Vector3: return 3;
			case EKind::Vector4: return 4;
			default:             return 0;
			}
		}
	}

	FString SplitTypeTag(const FString& Name, EKind& OutKind)
	{
		// The engine's function input tags (GetInputTypeName), plus T and SM
		static const TPair<const TCHAR*, EKind> Tags[] = {
			{ TEXT("S"), EKind::Scalar }, { TEXT("V2"), EKind::Vector2 }, { TEXT("V3"), EKind::Vector3 }, { TEXT("V4"), EKind::Vector4 },
			{ TEXT("B"), EKind::Bool }, { TEXT("SB"), EKind::Bool },
			{ TEXT("T"), EKind::Texture }, { TEXT("T2d"), EKind::Texture }, { TEXT("TCube"), EKind::Texture }, { TEXT("T2dArr"), EKind::Texture },
			{ TEXT("TVol"), EKind::Texture }, { TEXT("TExt"), EKind::Texture },
			{ TEXT("MA"), EKind::MaterialAttributes }, { TEXT("SM"), EKind::ShadingModel } };

		OutKind = EKind::Unknown;
		const FString Trimmed = Name.TrimEnd();
		int32 Open = INDEX_NONE;
		if (!Trimmed.EndsWith(TEXT(")")) || !Trimmed.FindLastChar(TEXT('('), Open))
		{
			return Name;
		}
		const FString Tag = Trimmed.Mid(Open + 1, Trimmed.Len() - Open - 2).TrimStartAndEnd();
		for (const TPair<const TCHAR*, EKind>& Known : Tags)
		{
			if (Tag.Equals(Known.Key, ESearchCase::IgnoreCase))
			{
				OutKind = Known.Value;
				return Trimmed.Left(Open).TrimEnd();
			}
		}
		return Name;	// "(Tangent Space)" and the like are part of the name
	}

	EKind KindOf(UMaterialExpression* Source, int32 Pin)
	{
		if (!Source)
		{
			return EKind::Unknown;
		}
		// A collection parameter has one untyped output: the collection says whether it's a scalar or a vector
		if (const UMaterialExpressionCollectionParameter* Collection = Cast<UMaterialExpressionCollectionParameter>(Source))
		{
			// Matched by id (what the node stores) or name (in case the id is stale)
			const bool bVector = Collection->Collection && Collection->Collection->VectorParameters.ContainsByPredicate(
				[Collection](const FCollectionVectorParameter& P) { return P.Id == Collection->ParameterId || P.ParameterName == Collection->ParameterName; });
			return bVector ? EKind::Vector4 : EKind::Scalar;
		}
		return KindOfType(Source->GetOutputValueType(Pin));
	}

	EKind KindOfType(EMaterialValueType Type)
	{
		if (Type == MCT_Bool || Type == MCT_StaticBool)
		{
			return EKind::Bool;
		}
		if (Type & MCT_Texture)
		{
			return EKind::Texture;
		}
		if (Type == MCT_MaterialAttributes)
		{
			return EKind::MaterialAttributes;
		}
		if (Type == MCT_ShadingModel)
		{
			return EKind::ShadingModel;
		}
		switch (Type)
		{
		case MCT_Float:		// one channel, or an output with no mask (scalar parameters)
		case MCT_Float1:
		case MCT_LWCScalar:   return EKind::Scalar;
		case MCT_Float2:
		case MCT_LWCVector2:  return EKind::Vector2;
		case MCT_Float3:
		case MCT_LWCVector3:  return EKind::Vector3;
		case MCT_Float4:
		case MCT_LWCVector4:  return EKind::Vector4;
		default:              return EKind::Unknown;
		}
	}

	EMaterialValueType TypeOfKind(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Scalar:             return MCT_Float1;
		case EKind::Vector2:            return MCT_Float2;
		case EKind::Vector3:            return MCT_Float3;
		case EKind::Vector4:            return MCT_Float4;
		case EKind::Bool:               return MCT_StaticBool;
		case EKind::Texture:            return MCT_Texture;
		case EKind::MaterialAttributes: return MCT_MaterialAttributes;
		case EKind::ShadingModel:       return MCT_ShadingModel;
		default:                        return MCT_Unknown;
		}
	}

	EKind KindOfShaderType(UE::Shader::EValueType Type)
	{
		using UE::Shader::EValueComponentType;
		const UE::Shader::FValueTypeDescription& Description = UE::Shader::GetValueTypeDescription(Type);
		switch (Description.ComponentType)
		{
		case EValueComponentType::Bool:    return EKind::Bool;
		case EValueComponentType::Float:
		case EValueComponentType::Double:
		case EValueComponentType::Int:
		case EValueComponentType::Numeric: return KindForWidth(Description.NumComponents);
		default:                           return EKind::Unknown;
		}
	}

	namespace
	{
		/**
		 * An engine input node (positions, vectors, depths, colors, ...): the name a reroute off it takes, as the
		 * node's caption reads (null: keep its pin names), and its width when it reports a plain float (Unknown: its
		 * pins' channel masks already say it).
		 */
		struct FEngineInput
		{
			UClass* Class;
			const TCHAR* Name;
			EKind Kind;
		};

		const FEngineInput* FindEngineInput(const UMaterialExpression* E)
		{
			static const TArray<FEngineInput> Inputs = {
				{ UMaterialExpressionTextureCoordinate::StaticClass(),  TEXT("UV"),                   EKind::Vector2 },	// + index
				{ UMaterialExpressionScreenPosition::StaticClass(),     nullptr,                      EKind::Vector2 },	// Viewport UV, Pixel Position
				{ UMaterialExpressionViewSize::StaticClass(),           TEXT("View Size"),            EKind::Vector2 },
				{ UMaterialExpressionWorldPosition::StaticClass(),      TEXT("World Position"),       EKind::Unknown },
				{ UMaterialExpressionObjectPositionWS::StaticClass(),   TEXT("Object Position"),      EKind::Vector3 },
				{ UMaterialExpressionActorPositionWS::StaticClass(),    TEXT("Actor Position"),       EKind::Vector3 },
				{ UMaterialExpressionCameraPositionWS::StaticClass(),   TEXT("Camera Position"),      EKind::Vector3 },
				{ UMaterialExpressionParticlePositionWS::StaticClass(), TEXT("Particle Position"),    EKind::Vector3 },
				{ UMaterialExpressionVertexNormalWS::StaticClass(),     TEXT("Vertex Normal"),        EKind::Vector3 },
				{ UMaterialExpressionPixelNormalWS::StaticClass(),      TEXT("Pixel Normal"),         EKind::Vector3 },
				{ UMaterialExpressionVertexTangentWS::StaticClass(),    TEXT("Vertex Tangent"),       EKind::Vector3 },
				{ UMaterialExpressionCameraVectorWS::StaticClass(),     TEXT("Camera Vector"),        EKind::Vector3 },
				{ UMaterialExpressionReflectionVectorWS::StaticClass(), TEXT("Reflection Vector"),    EKind::Vector3 },
				{ UMaterialExpressionLightVector::StaticClass(),        TEXT("Light Vector"),         EKind::Vector3 },
				{ UMaterialExpressionObjectBounds::StaticClass(),       TEXT("Object Bounds"),        EKind::Vector3 },
				{ UMaterialExpressionObjectOrientation::StaticClass(),  TEXT("Object Orientation"),   EKind::Vector3 },
				{ UMaterialExpressionObjectRadius::StaticClass(),       TEXT("Object Radius"),        EKind::Scalar },
				{ UMaterialExpressionTwoSidedSign::StaticClass(),       TEXT("Two Sided Sign"),       EKind::Scalar },
				{ UMaterialExpressionTime::StaticClass(),               TEXT("Time"),                 EKind::Scalar },
				{ UMaterialExpressionPixelDepth::StaticClass(),         TEXT("Pixel Depth"),          EKind::Unknown },
				{ UMaterialExpressionSceneDepth::StaticClass(),         TEXT("Scene Depth"),          EKind::Unknown },
				{ UMaterialExpressionPreSkinnedPosition::StaticClass(), TEXT("Pre-Skinned Position"), EKind::Unknown },
				{ UMaterialExpressionPreSkinnedNormal::StaticClass(),   TEXT("Pre-Skinned Normal"),   EKind::Unknown },
				{ UMaterialExpressionVertexColor::StaticClass(),        TEXT("Vertex Color"),         EKind::Unknown },
				{ UMaterialExpressionParticleColor::StaticClass(),      TEXT("Particle Color"),       EKind::Unknown },
				{ UMaterialExpressionParticleRadius::StaticClass(),     TEXT("Particle Radius"),      EKind::Scalar },
				{ UMaterialExpressionParticleSize::StaticClass(),       TEXT("Particle Size"),        EKind::Vector2 },	// width, height
				{ UMaterialExpressionParticleRelativeTime::StaticClass(), TEXT("Particle Relative Time"), EKind::Scalar },	// 0-1 over its life
				{ UMaterialExpressionParticleDirection::StaticClass(),  TEXT("Particle Direction"),   EKind::Vector3 },
				{ UMaterialExpressionParticleSpeed::StaticClass(),      TEXT("Particle Speed"),       EKind::Scalar },
				{ UMaterialExpressionParticleRandom::StaticClass(),     TEXT("Particle Random"),      EKind::Scalar },
				{ UMaterialExpressionParticleMotionBlurFade::StaticClass(), TEXT("Particle Motion Blur Fade"), EKind::Scalar },
				{ UMaterialExpressionParticleSpriteRotation::StaticClass(), TEXT("Particle Sprite Rotation"), EKind::Unknown },	// Rad / Deg pins (masked)
				{ UMaterialExpressionParticleMacroUV::StaticClass(),    TEXT("Particle Macro UV"),    EKind::Vector2 },
				{ UMaterialExpressionParticleSubUVProperties::StaticClass(), TEXT("Particle SubUV"), EKind::Unknown },	// UV0 / UV1 (masked), Blend
				{ UMaterialExpressionParticleSubUV::StaticClass(),      TEXT("Particle SubUV"),       EKind::Unknown },	// a sampler: pins masked
				{ UMaterialExpressionSphericalParticleOpacity::StaticClass(), TEXT("Spherical Particle Opacity"), EKind::Scalar },
				{ UMaterialExpressionDynamicParameter::StaticClass(),   TEXT("Dynamic Parameter"),    EKind::Unknown },	// pins masked
			};
			return Inputs.FindByPredicate([E](const FEngineInput& Input) { return E->IsA(Input.Class); });
		}

		/** Width an engine input node gives when it reports a plain float, else Unknown. */
		EKind FixedKindOf(const UMaterialExpression* E)
		{
			const FEngineInput* Input = FindEngineInput(E);
			return Input ? Input->Kind : EKind::Unknown;
		}

		/**
		 * One inference walk. Each (node, pin) is worked out once: branches that split and join again (common in big
		 * graphs) aren't walked twice, and a pin met again while still being worked out (a loop) counts as Unknown.
		 */
		class FInfer
		{
		public:
			EKind Pin(UMaterialExpression* E, int32 Pin)
			{
				if (!E)
				{
					return EKind::Unknown;
				}
				const TPair<const UMaterialExpression*, int32> Key(E, Pin);
				if (const EKind* Known = Done.Find(Key))
				{
					return *Known;
				}
				Done.Add(Key, EKind::Unknown);	// in progress
				const EKind Kind = Compute(E, Pin);
				Done.Add(Key, Kind);
				return Kind;
			}

			/** What a link carries: its mask if it has one, else what its source pin gives. */
			EKind Link(const FExpressionInput& Input)
			{
				if (!Input.Expression)
				{
					return EKind::Unknown;
				}
				return Input.Mask ? KindForWidth(VrrGraph::MaskWidth(Input)) : Pin(Input.Expression, Input.OutputIndex);
			}

		private:
			TMap<TPair<const UMaterialExpression*, int32>, EKind> Done;

			EKind Compute(UMaterialExpression* E, int32 OutputPin)
			{
				// Pass-throughs: follow them to the value they carry
				if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E))
				{
					return Usage->Declaration ? Link(Usage->Declaration->Input) : EKind::Unknown;
				}
				if (const UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(E))
				{
					return Link(Declaration->Input);
				}
				if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(E))
				{
					return Link(Reroute->Input);
				}
				if (const UMaterialExpressionVertexInterpolator* Interpolator = Cast<UMaterialExpressionVertexInterpolator>(E))
				{
					return Link(Interpolator->Input);	// PS gives what VS was fed, interpolated
				}
				// The output's name says its type: "Light Mask (S)", "Base Color (V3)"
				const TArray<FExpressionOutput>& Outputs = E->GetOutputs();
				if (Outputs.IsValidIndex(OutputPin) && !Outputs[OutputPin].OutputName.IsNone())
				{
					EKind Tagged = EKind::Unknown;
					SplitTypeTag(Outputs[OutputPin].OutputName.ToString(), Tagged);
					if (Tagged != EKind::Unknown)
					{
						return Tagged;
					}
				}
				if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(E))
				{
					const UMaterialExpressionFunctionOutput* Output = Call->FunctionOutputs.IsValidIndex(OutputPin) ? Call->FunctionOutputs[OutputPin].ExpressionOutput.Get() : nullptr;
					if (Output && Output->A.Expression)
					{
						return Link(Output->A);
					}
				}

				// The node knows its type (parameters, samplers, masked outputs, VRR nodes, ...)
				// (a collection parameter says "float" either way: KindOf asks its collection)
				const EMaterialValueType Reported = E->GetOutputValueType(OutputPin);
				const bool bUntyped = Reported == MCT_Unknown;	// e.g. Static Switch Parameter: whatever it switches between
				if (E->IsA<UMaterialExpressionCollectionParameter>() || (Reported != MCT_Float && !bUntyped)
					|| (Outputs.IsValidIndex(OutputPin) && Outputs[OutputPin].Mask))
				{
					return KindOf(E, OutputPin);
				}

				// A generic float or untyped output: work it out from the node and its inputs
				if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(E))
				{
					return KindForWidth((Mask->R != 0) + (Mask->G != 0) + (Mask->B != 0) + (Mask->A != 0));
				}
				if (const UMaterialExpressionStaticComponentMaskParameter* MaskParameter = Cast<UMaterialExpressionStaticComponentMaskParameter>(E))
				{
					// The channels on by default (an instance may pick others)
					return KindForWidth(MaskParameter->DefaultR + MaskParameter->DefaultG + MaskParameter->DefaultB + MaskParameter->DefaultA);
				}
				if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
				{
					return KindForWidth(FMath::Min(WidthOf(Link(Append->A)) + WidthOf(Link(Append->B)), 4));
				}
				if (E->IsA<UMaterialExpressionDotProduct>() || E->IsA<UMaterialExpressionLength>() || E->IsA<UMaterialExpressionDistance>()
					|| E->IsA<UMaterialExpressionFresnel>())
				{
					return EKind::Scalar;
				}
				if (E->IsA<UMaterialExpressionCrossProduct>())
				{
					return EKind::Vector3;
				}
				const EKind Fixed = FixedKindOf(E);
				if (Fixed != EKind::Unknown)
				{
					return Fixed;
				}
				int32 Widest = 0;
				for (FExpressionInputIterator It{ E }; It; ++It)
				{
					Widest = FMath::Max(Widest, WidthOf(Link(*It.Input)));
				}
				if (Widest > 0)
				{
					return KindForWidth(Widest);
				}
				return bUntyped ? EKind::Unknown : EKind::Scalar;	// nothing wired: a float constant / scalar parameter
			}
		};

		/** Only channel letters ("RGB", "R", "XY", ...): what samplers, parameters and breaks call their pins, not a name. */
		bool IsChannelLabel(const FString& Name)
		{
			return Name.Len() <= 4 && Algo::AllOf(Name, [](TCHAR C) { return FCString::Strchr(TEXT("RGBAXYZW"), FChar::ToUpper(C)) != nullptr; });
		}

		/**
		 * The parameter a node's pins are named after: scalar / vector / static bool / static switch / static component
		 * mask / texture / collection parameters, and a plain Texture Sample reading a texture object parameter.
		 */
		FName ParameterNameOf(const UMaterialExpression* Source)
		{
			if (const UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Source))
			{
				// Static bool covers Static Switch Parameter (a switch names its output after the parameter too)
				const bool bValue = Parameter->IsA<UMaterialExpressionScalarParameter>() || Parameter->IsA<UMaterialExpressionVectorParameter>()
					|| Parameter->IsA<UMaterialExpressionStaticBoolParameter>() || Parameter->IsA<UMaterialExpressionStaticComponentMaskParameter>();
				return bValue ? Parameter->ParameterName : NAME_None;
			}
			if (const UMaterialExpressionTextureSampleParameter* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Source))
			{
				return Texture->ParameterName;
			}
			if (const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Source))
			{
				const UMaterialExpressionTextureObjectParameter* Object = Cast<UMaterialExpressionTextureObjectParameter>(Sample->TextureObject.Expression);
				return Object ? Object->ParameterName : NAME_None;
			}
			if (const UMaterialExpressionCollectionParameter* Collection = Cast<UMaterialExpressionCollectionParameter>(Source))
			{
				return Collection->ParameterName;	// the parameter picked from its collection
			}
			return NAME_None;
		}

		/** "Camera Relative <Name>" for a position node set to camera relative, else Name. */
		FString WithOrigin(const TCHAR* Name, EPositionOrigin Origin)
		{
			return Origin == EPositionOrigin::CameraRelative ? FString::Printf(TEXT("Camera Relative %s"), Name) : FString(Name);
		}

		/** An engine input node's pin: after the node, with its variant and, on a node with several pins, its channel. */
		FString EngineInputName(const FEngineInput& Input, const UMaterialExpression* Source, int32 Pin)
		{
			TArray<FExpressionOutput>& Outputs = const_cast<UMaterialExpression*>(Source)->GetOutputs();
			if (const UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Source))
			{
				return FString::Printf(TEXT("%s%d"), Input.Name, TexCoord->CoordinateIndex);	// "UV0": the UV channel it reads
			}
			if (const UMaterialExpressionWorldPosition* WorldPosition = Cast<UMaterialExpressionWorldPosition>(Source))
			{
				// Pins XYZ / XY / Z: "World Position", "World Position XY", "World Position Z"
				const EWorldPositionIncludedOffsets Offsets = WorldPosition->WorldPositionShaderOffset;
				const FString Base = Offsets == WPT_CameraRelative || Offsets == WPT_CameraRelativeNoOffsets ? TEXT("Camera Relative Position") : Input.Name;
				const FString Channels = Outputs.IsValidIndex(Pin) ? Outputs[Pin].OutputName.ToString() : FString();
				return Channels.IsEmpty() || Channels == TEXT("XYZ") ? Base : FString::Printf(TEXT("%s %s"), *Base, *Channels);
			}
			if (const UMaterialExpressionObjectPositionWS* Object = Cast<UMaterialExpressionObjectPositionWS>(Source))
			{
				const bool bLight = Object->Material && Object->Material->MaterialDomain == MD_LightFunction;
				return WithOrigin(bLight ? TEXT("Light Position") : Input.Name, Object->OriginType);
			}
			if (const UMaterialExpressionActorPositionWS* Actor = Cast<UMaterialExpressionActorPositionWS>(Source))
			{
				return WithOrigin(Input.Name, Actor->OriginType);
			}
			if (const UMaterialExpressionParticlePositionWS* Particle = Cast<UMaterialExpressionParticlePositionWS>(Source))
			{
				return WithOrigin(Input.Name, Particle->OriginType);
			}
			if (Source->IsA<UMaterialExpressionParticleSpriteRotation>() || Source->IsA<UMaterialExpressionParticleSubUVProperties>())
			{
				// Named pins: "Particle Sprite Rotation Rad" / "... Deg", "Particle SubUV UV0" / "... UV1" / "... Blend"
				FString PinName = Outputs.IsValidIndex(Pin) ? Outputs[Pin].OutputName.ToString() : FString();
				PinName.ReplaceInline(TEXT("TextureCoordinate"), TEXT("UV"));
				return PinName.IsEmpty() ? FString(Input.Name) : FString::Printf(TEXT("%s %s"), Input.Name, *PinName);
			}
			// Dynamic Parameter slot 0 is "Dynamic Parameter", others "Dynamic Parameter <slot>"
			const UMaterialExpressionDynamicParameter* Dynamic = Cast<UMaterialExpressionDynamicParameter>(Source);
			const FString Base = Dynamic && Dynamic->ParameterIndex ? FString::Printf(TEXT("%s %u"), Input.Name, Dynamic->ParameterIndex) : FString(Input.Name);
			// A single channel of a node with several pins: its custom name (Dynamic Parameter's Param Names), else "Vertex Color R"
			const int32 Channel = Outputs.Num() > 1 && Outputs.IsValidIndex(Pin) ? VrrGraph::SingleChannelOf(Outputs[Pin]) : INDEX_NONE;
			return Channel == INDEX_NONE ? Base : VrrGraph::ChannelName(Source, Base, Channel);
		}

		/** A named reroute's name without its type prefix: "vec3_Normal" -> "Normal" (see MatchPrefix). */
		FString WithoutPrefix(const FString& Name)
		{
			return Name.RightChop(MatchPrefix(Name).Length);
		}

		/** Name of the value a link carries, through reroutes and named reroutes (the reroute's name, prefix dropped). */
		FString NameOfLink(const FExpressionInput& Link)
		{
			const UMaterialExpression* E = Link.Expression;
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(E))
			{
				return NameOfLink(Reroute->Input);
			}
			const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E);
			const UMaterialExpressionNamedRerouteDeclaration* Declaration = Usage ? Usage->Declaration.Get() : Cast<UMaterialExpressionNamedRerouteDeclaration>(E);
			if (Declaration)
			{
				return WithoutPrefix(Declaration->Name.ToString());
			}
			return NameOfOutput(E, Link.OutputIndex);
		}
	}

	FString NameOfOutput(const UMaterialExpression* Source, int32 Pin)
	{
		if (!Source)
		{
			return FString();
		}

		// 0. Vertex Interpolator: after what feeds its VS input, else "Vertex Interpolator"
		if (const UMaterialExpressionVertexInterpolator* Interpolator = Cast<UMaterialExpressionVertexInterpolator>(Source))
		{
			const FString Fed = NameOfLink(Interpolator->Input);
			return Fed.IsEmpty() ? TEXT("Vertex Interpolator") : Fed;
		}
		TArray<FExpressionOutput>& Outputs = const_cast<UMaterialExpression*>(Source)->GetOutputs();

		// 1. Parameters: the parameter name, a single channel its custom channel name else "<Name> R"
		const FName ParameterName = ParameterNameOf(Source);
		if (!ParameterName.IsNone())
		{
			const int32 Channel = Outputs.IsValidIndex(Pin) ? VrrGraph::SingleChannelOf(Outputs[Pin]) : INDEX_NONE;
			return Channel == INDEX_NONE ? ParameterName.ToString() : VrrGraph::ChannelName(Source, ParameterName.ToString(), Channel);
		}

		// 2. Engine input nodes: after the node
		if (const FEngineInput* Input = FindEngineInput(Source); Input && Input->Name)
		{
			return EngineInputName(*Input, Source, Pin);
		}

		// 3. Named pins (function calls, custom and VRR nodes) and function input nodes (their pin is unnamed: the node
		//    carries the name), type tag dropped
		const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Source);
		const FName Raw = FunctionInput ? FunctionInput->InputName : Outputs.IsValidIndex(Pin) ? Outputs[Pin].OutputName : NAME_None;
		if (Raw.IsNone())
		{
			return FString();
		}
		EKind Tagged;
		const FString Name = SplitTypeTag(Raw.ToString(), Tagged);
		return IsChannelLabel(Name) ? FString() : Name;
	}

	EKind InferKind(UMaterialExpression* Source, int32 Pin)
	{
		return FInfer().Pin(Source, Pin);
	}

	EKind InferLinkKind(const FExpressionInput& Link)
	{
		return FInfer().Link(Link);
	}

	namespace
	{
		/** The project's style for a kind (Project Settings > Plugins > Material Reroute Tools), null for Unknown. */
		const FVrrRerouteStyle* StyleOf(EKind Kind)
		{
			const UVrrRerouteSettings& Settings = UVrrRerouteSettings::Get();
			switch (Kind)
			{
			case EKind::Bool:               return &Settings.Bool;
			case EKind::Scalar:             return &Settings.Scalar;
			case EKind::Vector2:            return &Settings.Vector2;
			case EKind::Vector3:            return &Settings.Vector3;
			case EKind::Vector4:            return &Settings.Vector4;
			case EKind::Texture:            return &Settings.Texture;
			case EKind::MaterialAttributes: return &Settings.MaterialAttributes;
			case EKind::ShadingModel:       return &Settings.ShadingModel;
			default:                        return nullptr;
			}
		}
	}

	const TCHAR* Prefix(EKind Kind)
	{
		const FVrrRerouteStyle* Style = StyleOf(Kind);
		return Style ? *Style->Prefix : TEXT("");
	}

	FLinearColor Color(EKind Kind)
	{
		const FVrrRerouteStyle* Style = StyleOf(Kind);
		return Style ? Style->Color : UVrrRerouteSettings::Get().UnknownColor;
	}

	FPrefixMatch MatchPrefix(const FString& Name)
	{
		constexpr EKind Typed[] = { EKind::Bool, EKind::Scalar, EKind::Vector2, EKind::Vector3, EKind::Vector4, EKind::Texture, EKind::MaterialAttributes, EKind::ShadingModel };
		const TCHAR* const Defaults[] = { VrrDefaultPrefix::Bool, VrrDefaultPrefix::Scalar, VrrDefaultPrefix::Vector2, VrrDefaultPrefix::Vector3,
			VrrDefaultPrefix::Vector4, VrrDefaultPrefix::Texture, VrrDefaultPrefix::MaterialAttributes, VrrDefaultPrefix::ShadingModel };
		static_assert(UE_ARRAY_COUNT(Typed) == UE_ARRAY_COUNT(Defaults));

		// Longest match wins within each set, so "sm_" isn't read as "s_"
		FPrefixMatch Match;
		auto Consider = [&Name, &Match](const TCHAR* Candidate, EKind Kind)
		{
			const int32 Length = FCString::Strlen(Candidate);
			if (Length > Match.Length && Name.StartsWith(Candidate, ESearchCase::CaseSensitive))
			{
				Match = { Kind, Length };
			}
		};
		for (EKind Kind : Typed)
		{
			Consider(Prefix(Kind), Kind);
		}
		if (Match.Length == 0)
		{
			for (int32 i = 0; i < UE_ARRAY_COUNT(Typed); ++i)
			{
				Consider(Defaults[i], Typed[i]);
			}
		}
		if (Match.Length == 0)
		{
			for (const FString& Old : UVrrRerouteSettings::Get().OldPrefixes)
			{
				Consider(*Old, EKind::Unknown);
			}
		}
		return Match;
	}

	bool IsNormalSampler(const UMaterialExpression* E)
	{
		auto IsNormal = [](EMaterialSamplerType Type) { return Type == SAMPLERTYPE_Normal || Type == SAMPLERTYPE_VirtualNormal; };
		const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(E);
		if (!Sample || E->IsA<UMaterialExpressionTextureObjectParameter>())
		{
			return false;
		}
		if (IsNormal(Sample->SamplerType))
		{
			return true;
		}
		const UMaterialExpressionTextureSample* Object = Cast<UMaterialExpressionTextureSample>(Sample->TextureObject.Expression);
		return Object && IsNormal(Object->SamplerType);
	}

	FLinearColor ColorFor(const UMaterialExpression* Source, int32 Pin, EKind Kind)
	{
		if (Pin == 0 && WidthOf(Kind) > 0 && IsNormalSampler(Source))
		{
			return UVrrRerouteSettings::Get().NormalMapColor;	// RGB of a normal map
		}
		return Color(Kind);
	}

	FLinearColor ColorForLink(const FExpressionInput& Link, EKind Kind)
	{
		return Link.Mask ? Color(Kind) : ColorFor(Link.Expression, Link.OutputIndex, Kind);
	}
}
