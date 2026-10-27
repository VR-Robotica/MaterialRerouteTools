/*
 * Create Parameter Data Function: moves the selected material parameters into a new material function.
 *
 * Strategy
 *  Long material graphs collect dozens of parameters. This packs a selection of them into a material function (a
 *  reusable "parameter data" block) and leaves named reroutes in the material, so the rest of the graph reads them by
 *  name. Parameters inside a function are still material parameters with the same names, so instances keep working.
 *
 *  What moves (the "moved set"):
 *   - selected Scalar, Vector and Texture Sample parameters,
 *   - selected Texture Object parameters, together with the Texture Sample nodes they feed (a function can't output
 *     a texture, so an object that also feeds anything else is skipped),
 *   - Append nodes whose two inputs both come from moved nodes (repeated, so nested appends build up to a vector4).
 *
 *  What becomes an output: every moved node the rest of the material reads, plus selected parameters nothing reads
 *  (so they don't disappear). Functions can only output scalar / vector2 / vector3 / vector4, and the type comes from
 *  how the value is used:
 *   - scalar parameter -> S;  append -> its total width (2 scalars -> V2, ...);
 *   - texture sample / vector parameter: whole reads (RGB / RGBA pins, or masked to 2+ channels) share one output
 *     as wide as the channels they use (alpha -> V4, blue -> V3, only red/green -> V2), and every single channel
 *     that's read (its pin, or a whole pin masked to one channel) gets its own scalar output, named after the
 *     custom channel name (Parameter Customization) if set, else "<Name> R/G/B/A". Channels that only feed a moved
 *     append stay inside the function (the append is the output). Unused: V4 texture, V3 vector.
 *  Reads keep their channel mask (".R" of a V4 still reads R), except single-channel S outputs, which need none.
 *
 *  Links into the moved set from outside (e.g. a TexCoord into a sampler's UVs) become function inputs, and the call
 *  is wired to the original source, so nothing loses its connection.
 *
 *  The material editor edits a temporary copy of the material, so all material changes go to that copy (inside one
 *  undo transaction) and the graph is rebuilt; the user applies / saves as usual. The function is a new saved asset.
 *
 * Steps
 *  1. Collect the selected parameters and map who reads what in the material.
 *  2. Grow the moved set: texture samples of selected texture objects, then appends of moved values.
 *  3. Group into outputs (same-named parameters are one), pick each output's type, name and source pin.
 *  4. Ask where to save the function (defaults to PARAM_<Material> next to the material, "M_" prefix dropped).
 *  5. Build the function: copies of the moved nodes (relinked to each other), function inputs for outside links,
 *     outputs "<Name> (S|V2|V3|V4)", then save it.
 *  6. In the material: add the call (inputs wired to the original sources), a named reroute
 *     "s_|vec2_|vec3_|vec4_<Name>" per output, a reroute usage next to every read, then delete the moved nodes.
 *  7. Rebuild the graph and tell the material editor it changed.
 */

#include "Vrr_ParameterDataFunction.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "FileHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IMaterialEditor.h"
#include "MaterialEditingLibrary.h"
#include "MaterialEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionAntialiasedTextureMask.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCurveAtlasRowParameter.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "VrrParameterData"

namespace VrrParameterData
{
	enum class EType : uint8 { S, V2, V3, V4 };

	// Channel bits of the RGB / R / G / B / A / RGBA pins that vector parameters and texture samples share
	constexpr uint8 ChR = 1, ChG = 2, ChB = 4, ChA = 8;
	constexpr int32 PinRGB = 0, PinRGBA = 5;

	/** One place that reads a node: an expression input, or one of the material's own outputs (Consumer null). */
	struct FLink
	{
		UMaterialExpression* Consumer = nullptr;
		FExpressionInput* Input = nullptr;
	};

	/** One function output: the moved node it reads, how, and every outside read that now goes through it. */
	struct FOutput
	{
		UMaterialExpression* Source = nullptr;	// canonical moved node (in the material)
		FString Name;
		EType Type = EType::S;
		int32 SourcePin = 0;
		bool bMaskRG = false;					// vector2 from an RGB pin goes through an RG component mask
		bool bClearReadMasks = false;			// single-channel scalar: reads must not mask it again
		TArray<FLink> Reads;
		UMaterialExpressionNamedRerouteDeclaration* Declaration = nullptr;
	};

	/** A link from outside the moved set into a moved node: becomes a function input. */
	struct FOutsideInput
	{
		FString Name;
		FExpressionInput Source;				// the original link (expression, pin, mask)
	};

	const TCHAR* Suffix(EType Type)
	{
		switch (Type)
		{
		case EType::V2: return TEXT("(V2)");
		case EType::V3: return TEXT("(V3)");
		case EType::V4: return TEXT("(V4)");
		default:        return TEXT("(S)");
		}
	}

	const TCHAR* Prefix(EType Type)
	{
		switch (Type)
		{
		case EType::V2: return TEXT("vec2_");
		case EType::V3: return TEXT("vec3_");
		case EType::V4: return TEXT("vec4_");
		default:        return TEXT("s_");
		}
	}

	/** Reroute colors per type, so the type reads at a glance in the graph. */
	FLinearColor TypeColor(EType Type)
	{
		switch (Type)
		{
		case EType::V2: return FLinearColor(1.0f, 0.8f, 0.0f);
		case EType::V3: return FLinearColor(0.5f, 1.0f, 0.0f);
		case EType::V4: return FLinearColor(1.0f, 0.0f, 0.82f);
		default:        return FLinearColor(0.2f, 0.6f, 1.0f);
		}
	}

	EType TypeForWidth(int32 Width)
	{
		return Width <= 1 ? EType::S : Width == 2 ? EType::V2 : Width == 3 ? EType::V3 : EType::V4;
	}

	bool IsTextureObject(const UMaterialExpression* E) { return E->IsA<UMaterialExpressionTextureObjectParameter>(); }

	bool IsTextureSampleParameter(const UMaterialExpression* E)
	{
		return E->IsA<UMaterialExpressionTextureSampleParameter>() && !IsTextureObject(E) && !E->IsA<UMaterialExpressionAntialiasedTextureMask>();
	}

	/** Plain Texture Sample node (what a texture object feeds), not a parameter or a special sampler. */
	bool IsPlainSampler(const UMaterialExpression* E) { return E->GetClass() == UMaterialExpressionTextureSample::StaticClass(); }

	bool IsScalar(const UMaterialExpression* E) { return E->IsA<UMaterialExpressionScalarParameter>() && !E->IsA<UMaterialExpressionCurveAtlasRowParameter>(); }

	bool IsVectorParameter(const UMaterialExpression* E) { return E->GetClass() == UMaterialExpressionVectorParameter::StaticClass(); }

	/** Nodes with the shared RGB / R / G / B / A / RGBA pins. */
	bool IsVectorLike(const UMaterialExpression* E) { return IsVectorParameter(E) || IsTextureSampleParameter(E) || IsPlainSampler(E); }

	bool IsSelectableParameter(const UMaterialExpression* E)
	{
		return IsScalar(E) || IsVectorParameter(E) || IsTextureSampleParameter(E) || IsTextureObject(E);
	}

	uint8 PinChannels(int32 Pin)
	{
		switch (Pin)
		{
		case 1:  return ChR;
		case 2:  return ChG;
		case 3:  return ChB;
		case 4:  return ChA;
		case 5:  return ChR | ChG | ChB | ChA;
		default: return ChR | ChG | ChB;
		}
	}

	int32 CountChannels(uint8 Channels) { return FMath::CountBits(Channels); }

	/** Channels one read actually uses: the input's mask if it has one (on a whole pin), else what its pin gives. */
	uint8 ReadChannels(const FExpressionInput& Input)
	{
		const uint8 PinBits = PinChannels(Input.OutputIndex);
		if (Input.Mask && CountChannels(PinBits) > 1)
		{
			const uint8 MaskBits = (Input.MaskR ? ChR : 0) | (Input.MaskG ? ChG : 0) | (Input.MaskB ? ChB : 0) | (Input.MaskA ? ChA : 0);
			if (MaskBits & PinBits)
			{
				return MaskBits & PinBits;
			}
		}
		return PinBits;
	}

	/** Custom channel names (Parameter Customization) of a vector parameter or texture sample; a plain sampler with
	 *  none of its own uses its texture object parameter's. */
	FParameterChannelNames ChannelNamesOf(const UMaterialExpression* E)
	{
		auto HasAny = [](const FParameterChannelNames& N) { return !N.R.IsEmpty() || !N.G.IsEmpty() || !N.B.IsEmpty() || !N.A.IsEmpty(); };
		if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(E))
		{
			return Vector->ChannelNames;
		}
		if (const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(E))
		{
			if (!HasAny(Sample->ChannelNames) && IsPlainSampler(E))
			{
				if (const UMaterialExpressionTextureSample* Object = Cast<UMaterialExpressionTextureSample>(Sample->TextureObject.Expression))
				{
					return Object->ChannelNames;
				}
			}
			return Sample->ChannelNames;
		}
		return FParameterChannelNames();
	}

	int32 MaskWidth(const FExpressionInput& Input)
	{
		return (Input.MaskR ? 1 : 0) + (Input.MaskG ? 1 : 0) + (Input.MaskB ? 1 : 0) + (Input.MaskA ? 1 : 0);
	}

	/** Components a link carries: its mask if it has one, else what the source pin gives (0 = unknown). */
	int32 LinkWidth(const FExpressionInput& Input);

	int32 PinWidth(UMaterialExpression* E, int32 Pin)
	{
		if (IsScalar(E))
		{
			return 1;
		}
		if (IsVectorLike(E))
		{
			return CountChannels(PinChannels(Pin));
		}
		if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
		{
			return LinkWidth(Append->A) + LinkWidth(Append->B);
		}
		return 0;
	}

	int32 LinkWidth(const FExpressionInput& Input)
	{
		if (!Input.Expression)
		{
			return 0;
		}
		return Input.Mask ? MaskWidth(Input) : PinWidth(Input.Expression, Input.OutputIndex);
	}

	/** Function input type for a link from outside: from its mask / the source's output type, else the input's own type. */
	EFunctionInputType InputTypeFor(const FExpressionInput& Source, UMaterialExpression* ConsumerCopy, int32 ConsumerInputIndex)
	{
		auto FromValueType = [](EMaterialValueType Type, EFunctionInputType& Out) -> bool
		{
			switch (Type)
			{
			case MCT_Float1: case MCT_Float: case MCT_LWCScalar: Out = FunctionInput_Scalar; return true;
			case MCT_Float2: case MCT_LWCVector2:                Out = FunctionInput_Vector2; return true;
			case MCT_Float3: case MCT_LWCVector3:                Out = FunctionInput_Vector3; return true;
			case MCT_Float4: case MCT_LWCVector4:                Out = FunctionInput_Vector4; return true;
			case MCT_Texture2D:                                  Out = FunctionInput_Texture2D; return true;
			case MCT_TextureCube:                                Out = FunctionInput_TextureCube; return true;
			case MCT_Texture2DArray:                             Out = FunctionInput_Texture2DArray; return true;
			case MCT_VolumeTexture:                              Out = FunctionInput_VolumeTexture; return true;
			case MCT_TextureExternal:                            Out = FunctionInput_TextureExternal; return true;
			case MCT_StaticBool:                                 Out = FunctionInput_StaticBool; return true;
			case MCT_Bool:                                       Out = FunctionInput_Bool; return true;
			case MCT_MaterialAttributes:                         Out = FunctionInput_MaterialAttributes; return true;
			case MCT_Substrate:                                  Out = FunctionInput_Substrate; return true;
			default:                                             return false;
			}
		};

		EFunctionInputType Result = FunctionInput_Vector4;
		if (Source.Mask)
		{
			const int32 Width = MaskWidth(Source);
			return Width <= 1 ? FunctionInput_Scalar : Width == 2 ? FunctionInput_Vector2 : Width == 3 ? FunctionInput_Vector3 : FunctionInput_Vector4;
		}
		if (FromValueType(Source.Expression->GetOutputValueType(Source.OutputIndex), Result))
		{
			return Result;
		}
		if (FromValueType(ConsumerCopy->GetInputValueType(ConsumerInputIndex), Result))
		{
			return Result;
		}
		return FunctionInput_Vector4;
	}

	TSharedPtr<IMaterialEditor> GetEditor(const UMaterialGraph* Graph)
	{
		return FMaterialEditorUtilities::GetIMaterialEditorForObject(Graph);
	}

	void Notify(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bSuccess ? 6.0f : 10.0f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	TArray<UMaterialExpression*> GetSelectedParameters(const UMaterialGraph* Graph)
	{
		TArray<UMaterialExpression*> Result;
		const TSharedPtr<IMaterialEditor> Editor = GetEditor(Graph);
		if (!Editor.IsValid())
		{
			return Result;
		}
		for (UObject* Selected : Editor->GetSelectedNodes())
		{
			const UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Selected);
			UMaterialExpression* Expression = Node ? Node->MaterialExpression.Get() : nullptr;
			if (Expression && IsSelectableParameter(Expression))
			{
				Result.Add(Expression);
			}
		}
		return Result;
	}

	/** Longest shared start of the names, trimmed of trailing separators and axis letters ("Tiling X", "Tiling Y" -> "Tiling"). */
	FString CommonName(const TArray<FString>& Names)
	{
		if (Names.IsEmpty())
		{
			return FString();
		}
		FString Prefix = Names[0];
		for (const FString& Name : Names)
		{
			int32 Length = 0;
			while (Length < Prefix.Len() && Length < Name.Len() && Prefix[Length] == Name[Length])
			{
				++Length;
			}
			Prefix.LeftInline(Length);
		}
		// Drop a dangling axis letter only when it follows a separator ("Tiling X" -> "Tiling "), then trim separators
		if (Prefix.Len() >= 2 && FChar::IsWhitespace(Prefix[Prefix.Len() - 2]) && FString(TEXT("XYZWRGBAxyzwrgba")).Contains(Prefix.Right(1)))
		{
			Prefix.LeftChopInline(1);
		}
		while (!Prefix.IsEmpty() && FString(TEXT(" _-.:")).Contains(Prefix.Right(1)))
		{
			Prefix.LeftChopInline(1);
		}
		return Prefix.Len() >= 2 ? Prefix : FString::Join(Names, TEXT("_"));
	}

	/** Asks where to save the function. Returns false if cancelled. */
	bool AskForFunctionPath(const UObject* OriginalMaterial, FString& OutPackagePath, FString& OutAssetName)
	{
		const FString MaterialPackage = OriginalMaterial->GetOutermost()->GetName();

		FSaveAssetDialogConfig Config;
		Config.DialogTitleOverride = LOCTEXT("SaveTitle", "Save Parameter Data Function");
		Config.DefaultPath = FPackageName::GetLongPackagePath(MaterialPackage);
		// PARAM_<Material>, without the material's own "M_" prefix (M_Toon_Base -> PARAM_Toon_Base)
		FString MaterialName = FPackageName::GetShortName(MaterialPackage);
		MaterialName.RemoveFromStart(TEXT("M_"));
		Config.DefaultAssetName = TEXT("PARAM_") + MaterialName;
		Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;

		IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
		const FString ObjectPath = ContentBrowser.CreateModalSaveAssetDialog(Config);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		OutPackagePath = FPackageName::GetLongPackagePath(PackageName);
		OutAssetName = FPackageName::GetShortName(PackageName);
		return true;
	}

	void CreateFromSelection(UMaterialGraph* Graph)
	{
		UMaterial* Material = Graph ? Graph->Material.Get() : nullptr;
		const TSharedPtr<IMaterialEditor> Editor = GetEditor(Graph);
		if (!Material || !Editor.IsValid() || Graph->MaterialFunction)
		{
			return;
		}

		// 1. Selected parameters (top to bottom, as they sit in the graph) and who reads what
		TArray<UMaterialExpression*> Selected = GetSelectedParameters(Graph);
		if (Selected.IsEmpty())
		{
			Notify(LOCTEXT("NothingSelected", "Create Parameter Data Function: select parameters first."), false);
			return;
		}
		auto ByPosition = [](const UMaterialExpression& A, const UMaterialExpression& B)
		{
			return A.MaterialExpressionEditorY != B.MaterialExpressionEditorY ? A.MaterialExpressionEditorY < B.MaterialExpressionEditorY : A.MaterialExpressionEditorX < B.MaterialExpressionEditorX;
		};
		Selected.Sort(ByPosition);

		TMap<UMaterialExpression*, TArray<FLink>> Readers;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression)
			{
				continue;
			}
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				if (It.Input->Expression)
				{
					Readers.FindOrAdd(It.Input->Expression).Add({ Expression, It.Input });
				}
			}
		}
		{
			// The material's own outputs, hidden ones too (legacy pins while Substrate is on can still hold a link)
			TSet<FExpressionInput*> Seen;
			for (int32 Property = 0; Property < MP_MAX; ++Property)
			{
				FMaterialInputDescription Description;
				FExpressionInput* Input = Material->GetExpressionInputDescription(static_cast<EMaterialProperty>(Property), Description) ? Description.Input : nullptr;
				if (Input && Input->Expression && !Seen.Contains(Input))
				{
					Seen.Add(Input);
					Readers.FindOrAdd(Input->Expression).Add({ nullptr, Input });
				}
			}
		}
		auto ReadersOf = [&Readers](UMaterialExpression* E) -> const TArray<FLink>&
		{
			static const TArray<FLink> None;
			const TArray<FLink>* Found = Readers.Find(E);
			return Found ? *Found : None;
		};

		// 2. Moved set: the parameters, texture samples of selected texture objects, then appends of moved values
		TArray<UMaterialExpression*> Moved;
		TSet<UMaterialExpression*> MovedSet;
		TArray<FString> Skipped;
		auto AddMoved = [&Moved, &MovedSet](UMaterialExpression* E) { if (!MovedSet.Contains(E)) { MovedSet.Add(E); Moved.Add(E); } };

		for (UMaterialExpression* Parameter : Selected)
		{
			if (!IsTextureObject(Parameter))
			{
				AddMoved(Parameter);
				continue;
			}
			// 2a. A texture object can only move with its samplers, and only if it feeds nothing else
			TArray<UMaterialExpression*> Samplers;
			bool bOnlySamplers = true;
			for (const FLink& Link : ReadersOf(Parameter))
			{
				UMaterialExpressionTextureSample* Sampler = Link.Consumer && IsPlainSampler(Link.Consumer) ? Cast<UMaterialExpressionTextureSample>(Link.Consumer) : nullptr;
				if (Sampler && Link.Input == &Sampler->TextureObject)
				{
					Samplers.AddUnique(Sampler);
				}
				else
				{
					bOnlySamplers = false;
				}
			}
			if (!bOnlySamplers || Samplers.IsEmpty())
			{
				Skipped.Add(FString::Printf(TEXT("%s (%s)"), *Parameter->GetParameterName().ToString(),
					Samplers.IsEmpty() ? TEXT("feeds no Texture Sample") : TEXT("also feeds nodes other than Texture Samples")));
				continue;
			}
			AddMoved(Parameter);
			Samplers.Sort(ByPosition);
			for (UMaterialExpression* Sampler : Samplers)
			{
				AddMoved(Sampler);
			}
		}

		for (bool bGrew = true; bGrew; )
		{
			// 2b. Appends of two moved values, repeated so appends of appends join too (up to 4 components)
			bGrew = false;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression);
				if (Append && !MovedSet.Contains(Append)
					&& MovedSet.Contains(Append->A.Expression) && MovedSet.Contains(Append->B.Expression)
					&& PinWidth(Append, 0) > 0 && PinWidth(Append, 0) <= 4)
				{
					AddMoved(Append);
					bGrew = true;
				}
			}
		}
		if (Moved.IsEmpty())
		{
			Notify(FText::Format(LOCTEXT("NothingToMove", "Create Parameter Data Function: nothing to move.\nSkipped: {0}"), FText::FromString(FString::Join(Skipped, TEXT(", ")))), false);
			return;
		}

		// 3. Outputs. Same-named parameters are one parameter: the first one is "canonical", the others map to it.
		TMap<FName, UMaterialExpression*> ParameterByName;
		TMap<UMaterialExpression*, UMaterialExpression*> CanonicalOf;
		for (UMaterialExpression* E : Moved)
		{
			const FName Name = E->GetParameterName();
			if (Name.IsNone() || !E->HasAParameterName())
			{
				CanonicalOf.Add(E, E);
				continue;
			}
			if (UMaterialExpression** Existing = ParameterByName.Find(Name))
			{
				if ((*Existing)->GetClass() != E->GetClass())
				{
					Notify(FText::Format(LOCTEXT("MixedTypes", "Create Parameter Data Function: '{0}' is the name of two different kinds of parameter."), FText::FromName(Name)), false);
					return;
				}
				CanonicalOf.Add(E, *Existing);
			}
			else
			{
				ParameterByName.Add(Name, E);
				CanonicalOf.Add(E, E);
			}
		}

		// 3a. Reads from outside the moved set, per canonical node
		TMap<UMaterialExpression*, TArray<FLink>> OutsideReads;
		TSet<UMaterialExpression*> ReadInside;
		for (UMaterialExpression* E : Moved)
		{
			for (const FLink& Link : ReadersOf(E))
			{
				if (Link.Consumer && MovedSet.Contains(Link.Consumer))
				{
					ReadInside.Add(CanonicalOf[E]);
				}
				else
				{
					OutsideReads.FindOrAdd(CanonicalOf[E]).Add(Link);
				}
			}
		}

		// 3b. One output per canonical node read from outside, plus selected parameters nothing reads at all
		TArray<FOutput> Outputs;
		TSet<FString> UsedNames;
		auto UniqueName = [&UsedNames](FString Name)
		{
			FString Candidate = Name;
			for (int32 Number = 2; UsedNames.Contains(Candidate); ++Number)
			{
				Candidate = FString::Printf(TEXT("%s %d"), *Name, Number);
			}
			UsedNames.Add(Candidate);
			return Candidate;
		};
		TFunction<void(UMaterialExpression*, TArray<FString>&)> LeafNames = [&LeafNames](UMaterialExpression* E, TArray<FString>& Out)
		{
			if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
			{
				LeafNames(Append->A.Expression, Out);
				LeafNames(Append->B.Expression, Out);
			}
			else if (IsPlainSampler(E))
			{
				Out.Add(Cast<UMaterialExpressionTextureSample>(E)->TextureObject.Expression->GetParameterName().ToString());
			}
			else
			{
				Out.Add(E->GetParameterName().ToString());
			}
		};

		for (UMaterialExpression* E : Moved)
		{
			if (CanonicalOf[E] != E || IsTextureObject(E))
			{
				continue;	// duplicates share their canonical node's output; texture objects can't be outputs
			}
			const TArray<FLink>* Reads = OutsideReads.Find(E);
			const bool bSelectedUnused = !Reads && !ReadInside.Contains(E) && Selected.Contains(E);
			if (!Reads && !bSelectedUnused)
			{
				continue;	// only used inside the function (e.g. a scalar that feeds an append)
			}

			// 3c. Base name: the parameter's, the texture object's for samplers, the append's description or its inputs' shared name
			FString BaseName;
			if (UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
			{
				TArray<FString> Names;
				LeafNames(Append, Names);
				BaseName = Append->Desc.IsEmpty() ? CommonName(Names) : Append->Desc;
			}
			else if (IsPlainSampler(E))
			{
				TArray<FString> Names;
				LeafNames(E, Names);
				BaseName = Names[0];
			}
			else
			{
				BaseName = E->GetParameterName().ToString();
			}
			const TArray<FLink> NoReads;
			const TArray<FLink>& AllReads = Reads ? *Reads : NoReads;

			auto AddOutput = [&](const FString& Name, EType Type, int32 Pin, const TArray<FLink>& OutputReads) -> FOutput&
			{
				FOutput& Output = Outputs.AddDefaulted_GetRef();
				Output.Source = E;
				Output.Name = UniqueName(Name);
				Output.Type = Type;
				Output.SourcePin = Pin;
				Output.Reads = OutputReads;
				return Output;
			};

			// 3d. Type and source pin from how it's read
			if (IsScalar(E))
			{
				AddOutput(BaseName, EType::S, 0, AllReads);
			}
			else if (Cast<UMaterialExpressionAppendVector>(E))
			{
				AddOutput(BaseName, TypeForWidth(PinWidth(E, 0)), 0, AllReads);
			}
			else if (AllReads.IsEmpty())
			{
				const EType Type = IsVectorParameter(E) ? EType::V3 : EType::V4;
				AddOutput(BaseName, Type, Type == EType::V4 ? PinRGBA : PinRGB, AllReads);
			}
			else
			{
				// 3e. Texture samples and vector parameters: whole reads (RGB / RGBA, or masked to 2+ channels) share one
				//     output as wide as the channels they use, and every single channel that's read (its pin, or a whole
				//     pin masked to one channel) gets its own scalar output, named after the custom channel name if set,
				//     else "<Name> R/G/B/A". Channels that only feed a moved append stay inside the function.
				TArray<FLink> WholeReads;
				TArray<FLink> ChannelReads[4];
				uint8 WholeChannels = 0;
				for (const FLink& Read : AllReads)
				{
					const uint8 Channels = ReadChannels(*Read.Input);
					if (CountChannels(Channels) == 1)
					{
						ChannelReads[FMath::CountTrailingZeros(uint32(Channels))].Add(Read);
					}
					else
					{
						WholeReads.Add(Read);
						WholeChannels |= Channels;
					}
				}
				if (!WholeReads.IsEmpty())
				{
					if (WholeChannels & ChA)
					{
						AddOutput(BaseName, EType::V4, PinRGBA, WholeReads);
					}
					else if (WholeChannels & ChB)
					{
						AddOutput(BaseName, EType::V3, PinRGB, WholeReads);
					}
					else
					{
						AddOutput(BaseName, EType::V2, PinRGB, WholeReads).bMaskRG = true;
					}
				}
				const FParameterChannelNames CustomNames = ChannelNamesOf(E);
				const FText* Custom[4] = { &CustomNames.R, &CustomNames.G, &CustomNames.B, &CustomNames.A };
				static const TCHAR* Letters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					if (!ChannelReads[Channel].IsEmpty())
					{
						const FString CustomName = Custom[Channel]->ToString().TrimStartAndEnd();
						const FString Name = CustomName.IsEmpty() ? FString::Printf(TEXT("%s %s"), *BaseName, Letters[Channel]) : CustomName;
						AddOutput(Name, EType::S, Channel + 1, ChannelReads[Channel]).bClearReadMasks = true;
					}
				}
			}
		}
		if (Outputs.IsEmpty())
		{
			Notify(LOCTEXT("NoOutputs", "Create Parameter Data Function: the selection has nothing the material reads."), false);
			return;
		}

		// 4. Where to save the function
		const TArray<UObject*>* EditingObjects = Editor->GetObjectsCurrentlyBeingEdited();
		const UObject* OriginalMaterial = EditingObjects && EditingObjects->Num() > 0 ? (*EditingObjects)[0] : Material;
		FString PackagePath, AssetName;
		if (!AskForFunctionPath(OriginalMaterial, PackagePath, AssetName))
		{
			return;
		}

		// 5. Build the function
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UMaterialFunction* Function = Cast<UMaterialFunction>(AssetTools.CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), NewObject<UMaterialFunctionFactoryNew>()));
		if (!Function)
		{
			Notify(LOCTEXT("CreateFailed", "Create Parameter Data Function: couldn't create the material function asset."), false);
			return;
		}
		Function->Description = FString::Printf(TEXT("Parameter data moved out of %s (Create Parameter Data Function)."), *OriginalMaterial->GetName());

		// 5a. Copies keep their layout, shifted so the right-most node sits left of the outputs
		int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32;
		for (const UMaterialExpression* E : Moved)
		{
			MinX = FMath::Min(MinX, E->MaterialExpressionEditorX);
			MinY = FMath::Min(MinY, E->MaterialExpressionEditorY);
			MaxX = FMath::Max(MaxX, E->MaterialExpressionEditorX);
		}
		const int32 ShiftX = -400 - MaxX;
		const int32 ShiftY = -MinY;

		TMap<UMaterialExpression*, UMaterialExpression*> CopyOf;
		for (UMaterialExpression* E : Moved)
		{
			if (CanonicalOf[E] != E)
			{
				continue;
			}
			UMaterialExpression* Copy = DuplicateObject(E, Function);
			Copy->Material = nullptr;
			Copy->Function = Function;
			Copy->GraphNode = nullptr;
			Copy->SubgraphExpression = nullptr;
			Copy->MaterialExpressionEditorX = E->MaterialExpressionEditorX + ShiftX;
			Copy->MaterialExpressionEditorY = E->MaterialExpressionEditorY + ShiftY;
			Copy->UpdateMaterialExpressionGuid(true, false);
			Function->GetExpressionCollection().AddExpression(Copy);
			CopyOf.Add(E, Copy);
		}

		// 5b. Relink the copies: links between moved nodes point at the copies, links from outside become inputs
		TArray<FOutsideInput> OutsideInputs;
		TArray<UMaterialExpressionFunctionInput*> FunctionInputs;
		for (UMaterialExpression* E : Moved)
		{
			UMaterialExpression* const* CopyPtr = CopyOf.Find(E);
			if (!CopyPtr)
			{
				continue;
			}
			UMaterialExpression* Copy = *CopyPtr;
			for (FExpressionInputIterator It{ Copy }; It; ++It)
			{
				UMaterialExpression* Linked = It.Input->Expression;
				if (!Linked)
				{
					continue;
				}
				if (MovedSet.Contains(Linked))
				{
					It.Input->Expression = CopyOf[CanonicalOf[Linked]];
					continue;
				}

				// Same outside value (node, pin, mask) feeding several moved inputs shares one function input
				int32 Index = OutsideInputs.IndexOfByPredicate([&It](const FOutsideInput& O)
				{
					return O.Source.Expression == It.Input->Expression && O.Source.OutputIndex == It.Input->OutputIndex && O.Source.Mask == It.Input->Mask
						&& O.Source.MaskR == It.Input->MaskR && O.Source.MaskG == It.Input->MaskG && O.Source.MaskB == It.Input->MaskB && O.Source.MaskA == It.Input->MaskA;
				});
				if (Index == INDEX_NONE)
				{
					FString Owner = Copy->HasAParameterName() ? Copy->GetParameterName().ToString() : Copy->GetClass()->GetName().Replace(TEXT("MaterialExpression"), TEXT(""));
					if (IsPlainSampler(E))
					{
						TArray<FString> Names;
						LeafNames(E, Names);
						Owner = Names[0];
					}
					FOutsideInput& Outside = OutsideInputs.AddDefaulted_GetRef();
					Outside.Name = UniqueName(FString::Printf(TEXT("%s %s"), *Owner, *Copy->GetInputName(It.Index).ToString()));
					Outside.Source = *It.Input;

					UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(
						UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionInput::StaticClass(), Copy->MaterialExpressionEditorX - 350, Copy->MaterialExpressionEditorY));
					FunctionInput->InputName = FName(Outside.Name);
					FunctionInput->InputType = InputTypeFor(Outside.Source, Copy, It.Index);
					FunctionInput->SortPriority = OutsideInputs.Num() - 1;
					FunctionInput->Description = TEXT("Wired from the material by Create Parameter Data Function.");
					FunctionInputs.Add(FunctionInput);
					Index = OutsideInputs.Num() - 1;
				}
				// The call's input applies the original mask, so the copy reads the function input whole
				It.Input->Expression = FunctionInputs[Index];
				It.Input->OutputIndex = 0;
				It.Input->SetMask(0, 0, 0, 0, 0);
			}
		}

		// 5c. Outputs "<Name> (Type)", top to bottom like their sources
		Outputs.StableSort([](const FOutput& A, const FOutput& B) { return A.Source->MaterialExpressionEditorY < B.Source->MaterialExpressionEditorY; });
		TMap<UMaterialExpression*, int32> OutputsPerSource;	// stack a node's outputs (e.g. R, G, A) instead of overlapping them
		for (int32 i = 0; i < Outputs.Num(); ++i)
		{
			FOutput& Output = Outputs[i];
			UMaterialExpression* Copy = CopyOf[Output.Source];
			const int32 OutputY = Copy->MaterialExpressionEditorY + OutputsPerSource.FindOrAdd(Output.Source)++ * 90;
			UMaterialExpression* From = Copy;
			int32 FromPin = Output.SourcePin;
			if (Output.bMaskRG)
			{
				UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionComponentMask::StaticClass(), -200, OutputY));
				Mask->R = true;
				Mask->G = true;
				Mask->B = false;
				Mask->A = false;
				Mask->Input.Connect(Output.SourcePin, Copy);
				From = Mask;
				FromPin = 0;
			}
			UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(
				UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionOutput::StaticClass(), 0, OutputY));
			FunctionOutput->OutputName = FName(FString::Printf(TEXT("%s %s"), *Output.Name, Suffix(Output.Type)));
			FunctionOutput->Description = Output.Source->Desc;
			FunctionOutput->SortPriority = i;
			FunctionOutput->A.Connect(FromPin, From);
		}

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		Function->MarkPackageDirty();
		UEditorLoadingAndSavingUtils::SavePackages({ Function->GetOutermost() }, false);

		// 6. Rewire the material (the editor's working copy), as one undo step
		{
			const FScopedTransaction Transaction(LOCTEXT("Transaction", "Create Parameter Data Function"));
			Material->Modify();

			// 6a. The call where the moved nodes were; its inputs read what the moved nodes used to read
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
				UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, nullptr, UMaterialExpressionMaterialFunctionCall::StaticClass(), Function, MinX, MinY, false));
			if (!Call || Call->MaterialFunction != Function)
			{
				Notify(LOCTEXT("CallFailed", "Create Parameter Data Function: couldn't place the function in the material. The function asset was created."), false);
				return;
			}
			for (const FOutsideInput& Outside : OutsideInputs)
			{
				for (FFunctionExpressionInput& CallInput : Call->FunctionInputs)
				{
					if (CallInput.ExpressionInput && CallInput.ExpressionInput->InputName == FName(Outside.Name))
					{
						CallInput.Input.Expression = Outside.Source.Expression;
						CallInput.Input.OutputIndex = Outside.Source.OutputIndex;
						CallInput.Input.SetMask(Outside.Source.Mask, Outside.Source.MaskR, Outside.Source.MaskG, Outside.Source.MaskB, Outside.Source.MaskA);
					}
				}
			}

			// 6b. One named reroute per output
			const int32 DeclarationX = MinX + 450;
			for (int32 i = 0; i < Outputs.Num(); ++i)
			{
				FOutput& Output = Outputs[i];
				const FName OutputName(FString::Printf(TEXT("%s %s"), *Output.Name, Suffix(Output.Type)));
				const int32 CallPin = Call->GetOutputs().IndexOfByPredicate([&OutputName](const FExpressionOutput& O) { return O.OutputName == OutputName; });
				if (CallPin == INDEX_NONE)
				{
					continue;
				}
				Output.Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(
					UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, nullptr, UMaterialExpressionNamedRerouteDeclaration::StaticClass(), nullptr, DeclarationX, MinY + i * 64, false));
				Output.Declaration->Input.Connect(CallPin, Call);
				Output.Declaration->NodeColor = TypeColor(Output.Type);
				Output.Declaration->SetEditableName(FString::Printf(TEXT("%s%s"), Prefix(Output.Type), *Output.Name));	// made unique if taken
			}

			// 6c. A usage next to every outside read. Reads keep their channel mask, except single-channel scalars.
			TMap<UMaterialExpression*, int32> UsagesPerConsumer;
			for (FOutput& Output : Outputs)
			{
				if (!Output.Declaration)
				{
					continue;
				}
				for (const FLink& Read : Output.Reads)
				{
					const int32 Slot = UsagesPerConsumer.FindOrAdd(Read.Consumer)++;
					const int32 X = (Read.Consumer ? Read.Consumer->MaterialExpressionEditorX : Material->EditorX) - 300;
					const int32 Y = (Read.Consumer ? Read.Consumer->MaterialExpressionEditorY : Material->EditorY) + Slot * 48;

					UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(
						UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, nullptr, UMaterialExpressionNamedRerouteUsage::StaticClass(), nullptr, X, Y, false));
					Usage->Declaration = Output.Declaration;
					Usage->DeclarationGuid = Output.Declaration->VariableGuid;

					if (Read.Consumer)
					{
						Read.Consumer->Modify();
					}
					Read.Input->Expression = Usage;
					Read.Input->OutputIndex = 0;
					if (Output.bClearReadMasks)
					{
						Read.Input->SetMask(0, 0, 0, 0, 0);
					}
				}
			}

			// 6d. The moved nodes now live in the function
			for (UMaterialExpression* E : Moved)
			{
				E->Modify();
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, E);
			}
		}

		// 7. Show the new nodes and mark the material as changed (Apply / Save as usual)
		Graph->RebuildGraph();
		Editor->UpdateMaterialAfterGraphChange();

		FText Message = FText::Format(LOCTEXT("Done", "Moved {0} node(s) into {1}: {2} output(s), {3} input(s)."),
			Moved.Num(), FText::FromString(AssetName), Outputs.Num(), OutsideInputs.Num());
		if (!Skipped.IsEmpty())
		{
			Message = FText::Format(LOCTEXT("DoneSkipped", "{0}\nSkipped: {1}"), Message, FText::FromString(FString::Join(Skipped, TEXT(", "))));
		}
		Notify(Message, true);
	}
}

#undef LOCTEXT_NAMESPACE
