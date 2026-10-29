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
 *  Add to Parameter Data Function does the same into a function the material already calls (a PARAM_ function, or
 *  one this tool made), through that call:
 *   - new nodes go below the function's content, new outputs / inputs after its existing ones (sort priority), so
 *     the call's existing pins and links don't move; output / input names never repeat one already in use,
 *   - a parameter the function already has (same name) isn't copied again: the function's node stands in for it,
 *     and if an output already gives the same pin and type, that output (and its reroute, if any) is reused,
 *   - new function inputs use their preview value as default, so other materials calling the function still
 *     compile without wiring them (they don't read the new outputs anyway),
 *   - the function asset is edited and saved directly, so it must not be open in its own editor.
 *
 * Steps
 *  1. Collect the selected parameters and map who reads what in the material.
 *  2. Grow the moved set: texture samples of selected texture objects, then appends of moved values.
 *  3. Group into outputs (same-named parameters are one), pick each output's type, name and source pin.
 *  4. Ask where to save the function (defaults to PARAM_<Material> next to the material, "M_" prefix dropped), or,
 *     when adding, use the target function (parameters it already has stand in for their copies, step 3').
 *  5. Build the function: copies of the moved nodes (relinked to each other), function inputs for outside links,
 *     outputs "<Name> (S|V2|V3|V4)", then save it.
 *  6. In the material: add the call or refresh the target call (new inputs wired to the original sources), a named
 *     reroute "s_|vec2_|vec3_|vec4_<Name>" per output (named and colored as in Vrr_RerouteStyle), a usage next to
 *     every read, then delete the moved nodes.
 *  7. Rebuild the graph, tell the material editor it changed, and select the call and its named reroute declarations.
 *
 * Each step is one method of FRun below, which holds the state the steps share.
 */

#include "Vrr_ParameterDataFunction.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "IContentBrowserSingleton.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "FileHelpers.h"
#include "IMaterialEditor.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
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
#include "Subsystems/AssetEditorSubsystem.h"

#define LOCTEXT_NAMESPACE "VrrParameterData"

namespace VrrParameterData
{
	enum class EType : uint8 { S, V2, V3, V4 };

	// Channel bits of the RGB / R / G / B / A / RGBA pins that vector parameters and texture samples share
	constexpr uint8 ChR = 1, ChG = 2, ChB = 4, ChA = 8;
	constexpr int32 PinRGB = 0, PinRGBA = 5;

	using VrrGraph::FLink;

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
		UMaterialExpressionFunctionOutput* ExistingOutput = nullptr;	// adding to a function: an output that already gives this value
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

	VrrReroute::EKind ToKind(EType Type)
	{
		switch (Type)
		{
		case EType::V2: return VrrReroute::EKind::Vector2;
		case EType::V3: return VrrReroute::EKind::Vector3;
		case EType::V4: return VrrReroute::EKind::Vector4;
		default:        return VrrReroute::EKind::Scalar;
		}
	}

	/** "Tiling (V2)" -> "Tiling": an output's name without its type suffix. */
	FString StripSuffix(const FString& OutputName)
	{
		for (EType Type : { EType::S, EType::V2, EType::V3, EType::V4 })
		{
			const FString Ending = FString(TEXT(" ")) + Suffix(Type);
			if (OutputName.EndsWith(Ending))
			{
				return OutputName.LeftChop(Ending.Len());
			}
		}
		return OutputName;
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
		return Input.Mask ? VrrGraph::MaskWidth(Input) : PinWidth(Input.Expression, Input.OutputIndex);
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
			const int32 Width = VrrGraph::MaskWidth(Source);
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

	/** Marker written into the Description of functions this tool creates. */
	const TCHAR* const MadeByTool = TEXT("(Create Parameter Data Function)");

	TArray<UMaterialExpression*> GetSelectedParameters(const UMaterialGraph* Graph)
	{
		return VrrGraph::GetSelectedExpressions(Graph, IsSelectableParameter);
	}

	TArray<UMaterialExpressionMaterialFunctionCall*> GetParameterDataCalls(const UMaterialGraph* Graph)
	{
		TArray<UMaterialExpressionMaterialFunctionCall*> Result;
		const UMaterial* Material = Graph ? Graph->Material.Get() : nullptr;
		if (!Material)
		{
			return Result;
		}
		TSet<const UMaterialFunction*> Seen;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			const UMaterialFunction* Function = Call ? Cast<UMaterialFunction>(Call->MaterialFunction) : nullptr;	// not function instances
			if (Function && !Seen.Contains(Function)
				&& (Function->GetName().StartsWith(TEXT("PARAM_")) || Function->Description.Contains(MadeByTool)))
			{
				Seen.Add(Function);
				Result.Add(Call);
			}
		}
		Result.Sort([](const UMaterialExpressionMaterialFunctionCall& A, const UMaterialExpressionMaterialFunctionCall& B)
		{
			return A.MaterialFunction->GetName() < B.MaterialFunction->GetName();
		});
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

	/** Everything already in a function the selection is being added to. */
	struct FExistingFunction
	{
		TMap<FName, UMaterialExpression*> Parameters;			// by parameter name
		TArray<UMaterialExpressionFunctionOutput*> Outputs;
		TSet<FString> Names;									// output (without type suffix) and input names in use
		int32 MaxOutputPriority = -1;
		int32 MaxInputPriority = -1;
		int32 OutputX = 0;										// where the outputs sit (new ones line up with them)
		int32 BottomY = 0;										// lowest node, new nodes go below it

		explicit FExistingFunction(const UMaterialFunction* Function)
		{
			bool bAny = false;
			for (UMaterialExpression* E : Function->GetExpressions())
			{
				if (!E)
				{
					continue;
				}
				BottomY = bAny ? FMath::Max(BottomY, E->MaterialExpressionEditorY) : E->MaterialExpressionEditorY;
				bAny = true;
				if (UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(E))
				{
					OutputX = Outputs.IsEmpty() ? Output->MaterialExpressionEditorX : FMath::Max(OutputX, Output->MaterialExpressionEditorX);
					Outputs.Add(Output);
					Names.Add(StripSuffix(Output->OutputName.ToString()));
					MaxOutputPriority = FMath::Max(MaxOutputPriority, Output->SortPriority);
				}
				else if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(E))
				{
					Names.Add(Input->InputName.ToString());
					MaxInputPriority = FMath::Max(MaxInputPriority, Input->SortPriority);
				}
				else if (E->HasAParameterName() && !E->GetParameterName().IsNone())
				{
					Parameters.FindOrAdd(E->GetParameterName(), E);
				}
			}
		}

		/** An output that already gives Node's Pin as Type (straight, or through the RG mask a V2 uses). */
		UMaterialExpressionFunctionOutput* FindOutput(const UMaterialExpression* Node, int32 Pin, bool bMaskRG, EType Type) const
		{
			for (UMaterialExpressionFunctionOutput* Output : Outputs)
			{
				const UMaterialExpression* From = Output->A.Expression;
				int32 FromPin = Output->A.OutputIndex;
				bool bThroughRG = false;
				if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(From))
				{
					if (!(Mask->R && Mask->G && !Mask->B && !Mask->A))
					{
						continue;
					}
					From = Mask->Input.Expression;
					FromPin = Mask->Input.OutputIndex;
					bThroughRG = true;
				}
				if (From == Node && FromPin == Pin && bThroughRG == bMaskRG && Output->OutputName.ToString().EndsWith(Suffix(Type)))
				{
					return Output;
				}
			}
			return nullptr;
		}
	};

	/** Name a moved parameter goes by: a plain sampler's texture object's, else its own parameter name. */
	FString ParameterNameOf(const UMaterialExpression* E)
	{
		if (IsPlainSampler(E))
		{
			return Cast<UMaterialExpressionTextureSample>(E)->TextureObject.Expression->GetParameterName().ToString();
		}
		return E->GetParameterName().ToString();
	}

	/** Parameter names an append is built from (its leaves, nested appends included). */
	void LeafNames(const UMaterialExpression* E, TArray<FString>& Out)
	{
		if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
		{
			LeafNames(Append->A.Expression, Out);
			LeafNames(Append->B.Expression, Out);
		}
		else
		{
			Out.Add(ParameterNameOf(E));
		}
	}

	/**
	 * One run of the tool: the state its steps share, and one method per step (the steps are listed at the top of the
	 * file). A step that returns false has already told the user why, and the run stops there.
	 */
	class FRun
	{
	public:
		/** Create (TargetCall null) or add to the function TargetCall calls. */
		FRun(UMaterialGraph* InGraph, UMaterialExpressionMaterialFunctionCall* InTargetCall);

		void Execute();

	private:
		// Setup
		UMaterialGraph* Graph = nullptr;
		UMaterial* Material = nullptr;
		TSharedPtr<IMaterialEditor> Editor;
		UMaterialExpressionMaterialFunctionCall* TargetCall = nullptr;
		bool bAdd = false;
		FText Tool;

		// 0. The function being added to (when creating, it's made in step 4)
		UMaterialFunction* Function = nullptr;
		TOptional<FExistingFunction> Existing;

		// 1. Selection and who reads what
		TArray<UMaterialExpression*> Selected;
		VrrGraph::FReaders Readers;

		// 2. Moved set
		TArray<UMaterialExpression*> Moved;
		TSet<UMaterialExpression*> MovedSet;
		TArray<FString> Skipped;

		// 3. Outputs
		TMap<FName, UMaterialExpression*> ParameterByName;
		TMap<UMaterialExpression*, UMaterialExpression*> CanonicalOf;
		TMap<UMaterialExpression*, UMaterialExpression*> AlreadyInFunction;	// adding: moved node -> the function's own node
		TArray<FOutput> Outputs;
		TSet<FString> UsedNames;		// output and input names in use (new ones never repeat one)
		int32 ReusedOutputs = 0;

		// 4-5. The function
		FString AssetName;
		int32 MinX = MAX_int32;			// top-left of the moved nodes, where the call goes
		int32 MinY = MAX_int32;
		int32 OutputX = 0;				// where the function's outputs sit
		TMap<UMaterialExpression*, UMaterialExpression*> CopyOf;	// canonical moved node -> its node in the function
		TArray<FOutsideInput> OutsideInputs;

		// 6. The call in the material
		UMaterialExpressionMaterialFunctionCall* Call = nullptr;

		void Fail(const FText& Message) const;
		const TArray<FLink>& ReadersOf(UMaterialExpression* E) const;
		FString UniqueName(const FString& Name);
		void AddMoved(UMaterialExpression* E);
		FOutput& AddOutput(UMaterialExpression* Source, const FString& Name, EType Type, int32 Pin, const TArray<FLink>& Reads);

		bool CheckTarget();							// 0
		bool CollectSelection();					// 1
		bool GrowMovedSet();						// 2
		void AddTextureObject(UMaterialExpression* Parameter);	// 2a
		bool MergeSameNamed();						// 3, 3'
		bool BuildOutputs();						// 3a-3d
		void AddVectorOutputs(UMaterialExpression* E, const FString& BaseName, const TArray<FLink>& Reads);	// 3e
		void ReuseExistingOutputs();				// 3f
		bool PrepareFunction();						// 4
		void CopyMovedNodes();						// 5a
		void LinkFunctionInputs();					// 5b
		void AddFunctionOutputs();					// 5c
		bool RewireMaterial();						// 6
		bool PlaceCall();							// 6a
		void AddDeclarations();						// 6b
		void AddUsages();							// 6c
		void Finish();								// 7
	};

	FRun::FRun(UMaterialGraph* InGraph, UMaterialExpressionMaterialFunctionCall* InTargetCall)
		: Graph(InGraph)
		, Material(InGraph ? InGraph->Material.Get() : nullptr)
		, Editor(VrrGraph::GetEditor(InGraph))
		, TargetCall(InTargetCall)
		, bAdd(InTargetCall != nullptr)
		, Tool(InTargetCall ? LOCTEXT("AddTool", "Add to Parameter Data Function") : LOCTEXT("CreateTool", "Create Parameter Data Function"))
	{
	}

	void FRun::Execute()
	{
		if (!Material || !Editor.IsValid() || Graph->MaterialFunction)
		{
			return;
		}
		if (!CheckTarget() || !CollectSelection() || !GrowMovedSet() || !MergeSameNamed() || !BuildOutputs())
		{
			return;
		}
		ReuseExistingOutputs();
		if (!PrepareFunction())
		{
			return;
		}
		CopyMovedNodes();
		LinkFunctionInputs();
		AddFunctionOutputs();
		if (RewireMaterial())
		{
			Finish();
		}
	}

	void FRun::Fail(const FText& Message) const
	{
		VrrGraph::Notify(FText::Format(LOCTEXT("Fail", "{0}: {1}"), Tool, Message), false);
	}

	const TArray<FLink>& FRun::ReadersOf(UMaterialExpression* E) const
	{
		static const TArray<FLink> None;
		const TArray<FLink>* Found = Readers.Find(E);
		return Found ? *Found : None;
	}

	FString FRun::UniqueName(const FString& Name)
	{
		FString Candidate = Name;
		for (int32 Number = 2; UsedNames.Contains(Candidate); ++Number)
		{
			Candidate = FString::Printf(TEXT("%s %d"), *Name, Number);
		}
		UsedNames.Add(Candidate);
		return Candidate;
	}

	void FRun::AddMoved(UMaterialExpression* E)
	{
		if (!MovedSet.Contains(E))
		{
			MovedSet.Add(E);
			Moved.Add(E);
		}
	}

	FOutput& FRun::AddOutput(UMaterialExpression* Source, const FString& Name, EType Type, int32 Pin, const TArray<FLink>& Reads)
	{
		FOutput& Output = Outputs.AddDefaulted_GetRef();
		Output.Source = Source;
		Output.Name = UniqueName(Name);
		Output.Type = Type;
		Output.SourcePin = Pin;
		Output.Reads = Reads;
		return Output;
	}

	// 0. Adding: the target must be a plain function that isn't open in its own editor (that copy would overwrite ours)
	bool FRun::CheckTarget()
	{
		if (!bAdd)
		{
			return true;
		}
		Function = Cast<UMaterialFunction>(TargetCall->MaterialFunction);
		if (!Function)
		{
			Fail(LOCTEXT("NotAFunction", "the target is not a material function."));
			return false;
		}
		if (GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Function, false))
		{
			Fail(FText::Format(LOCTEXT("FunctionOpen", "{0} is open in an editor. Save and close it, then try again."), FText::FromString(Function->GetName())));
			return false;
		}
		Existing.Emplace(Function);
		UsedNames = Existing->Names;
		return true;
	}

	// 1. Selected parameters (top to bottom, as they sit in the graph) and who reads what
	bool FRun::CollectSelection()
	{
		Selected = GetSelectedParameters(Graph);
		if (Selected.IsEmpty())
		{
			Fail(LOCTEXT("NothingSelected", "select parameters first."));
			return false;
		}
		Selected.Sort(VrrGraph::ByPosition);
		Readers = VrrGraph::MapReaders(Material);
		return true;
	}

	// 2. Moved set: the parameters, texture samples of selected texture objects, then appends of moved values
	bool FRun::GrowMovedSet()
	{
		for (UMaterialExpression* Parameter : Selected)
		{
			if (IsTextureObject(Parameter))
			{
				AddTextureObject(Parameter);
			}
			else
			{
				AddMoved(Parameter);
			}
		}

		// 2b. Appends of two moved values, repeated so appends of appends join too (up to 4 components)
		for (bool bGrew = true; bGrew; )
		{
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
			Fail(FText::Format(LOCTEXT("NothingToMove", "nothing to move.\nSkipped: {0}"), FText::FromString(FString::Join(Skipped, TEXT(", ")))));
			return false;
		}
		return true;
	}

	// 2a. A texture object can only move with its samplers, and only if it feeds nothing else
	void FRun::AddTextureObject(UMaterialExpression* Parameter)
	{
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
			return;
		}
		AddMoved(Parameter);
		Samplers.Sort(VrrGraph::ByPosition);
		for (UMaterialExpression* Sampler : Samplers)
		{
			AddMoved(Sampler);
		}
	}

	// 3. Same-named parameters are one parameter: the first one is "canonical", the others map to it.
	bool FRun::MergeSameNamed()
	{
		for (UMaterialExpression* E : Moved)
		{
			const FName Name = E->GetParameterName();
			if (Name.IsNone() || !E->HasAParameterName())
			{
				CanonicalOf.Add(E, E);
				continue;
			}
			if (UMaterialExpression** Earlier = ParameterByName.Find(Name))
			{
				if ((*Earlier)->GetClass() != E->GetClass())
				{
					Fail(FText::Format(LOCTEXT("MixedTypes", "'{0}' is the name of two different kinds of parameter."), FText::FromName(Name)));
					return false;
				}
				CanonicalOf.Add(E, *Earlier);
			}
			else
			{
				ParameterByName.Add(Name, E);
				CanonicalOf.Add(E, E);
			}
		}

		// 3'. Adding: a parameter the function already has isn't copied again, the function's own node stands in for it
		if (Existing)
		{
			for (const TPair<FName, UMaterialExpression*>& Pair : ParameterByName)
			{
				UMaterialExpression* const* Found = Existing->Parameters.Find(Pair.Key);
				if (!Found)
				{
					continue;
				}
				if ((*Found)->GetClass() != Pair.Value->GetClass())
				{
					Fail(FText::Format(LOCTEXT("MixedTypesExisting", "{0} already has a different kind of parameter named '{1}'."), FText::FromString(Function->GetName()), FText::FromName(Pair.Key)));
					return false;
				}
				AlreadyInFunction.Add(Pair.Value, *Found);
			}
		}
		return true;
	}

	bool FRun::BuildOutputs()
	{
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
			static const TArray<FLink> NoReads;
			const TArray<FLink>& AllReads = Reads ? *Reads : NoReads;

			// 3c. Base name: the parameter's (the texture object's for samplers), or the append's description or its
			//     inputs' shared name
			FString BaseName;
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
			{
				TArray<FString> Names;
				LeafNames(Append, Names);
				BaseName = Append->Desc.IsEmpty() ? CommonName(Names) : Append->Desc;
			}
			else
			{
				BaseName = ParameterNameOf(E);
			}

			// 3d. Type and source pin from how it's read
			if (IsScalar(E))
			{
				AddOutput(E, BaseName, EType::S, 0, AllReads);
			}
			else if (E->IsA<UMaterialExpressionAppendVector>())
			{
				AddOutput(E, BaseName, TypeForWidth(PinWidth(E, 0)), 0, AllReads);
			}
			else if (AllReads.IsEmpty())
			{
				const EType Type = IsVectorParameter(E) ? EType::V3 : EType::V4;
				AddOutput(E, BaseName, Type, Type == EType::V4 ? PinRGBA : PinRGB, AllReads);
			}
			else
			{
				AddVectorOutputs(E, BaseName, AllReads);
			}
		}
		if (Outputs.IsEmpty())
		{
			Fail(LOCTEXT("NoOutputs", "the selection has nothing the material reads."));
			return false;
		}
		return true;
	}

	// 3e. Texture samples and vector parameters: whole reads (RGB / RGBA, or masked to 2+ channels) share one output as
	//     wide as the channels they use, and every single channel that's read (its pin, or a whole pin masked to one
	//     channel) gets its own scalar output, named after the custom channel name if set, else "<Name> R/G/B/A".
	//     Channels that only feed a moved append stay inside the function.
	void FRun::AddVectorOutputs(UMaterialExpression* E, const FString& BaseName, const TArray<FLink>& Reads)
	{
		TArray<FLink> WholeReads;
		TArray<FLink> ChannelReads[4];
		uint8 WholeChannels = 0;
		for (const FLink& Read : Reads)
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
				AddOutput(E, BaseName, EType::V4, PinRGBA, WholeReads);
			}
			else if (WholeChannels & ChB)
			{
				AddOutput(E, BaseName, EType::V3, PinRGB, WholeReads);
			}
			else
			{
				AddOutput(E, BaseName, EType::V2, PinRGB, WholeReads).bMaskRG = true;
			}
		}
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			if (!ChannelReads[Channel].IsEmpty())
			{
				AddOutput(E, VrrGraph::ChannelName(E, BaseName, Channel), EType::S, Channel + 1, ChannelReads[Channel]).bClearReadMasks = true;
			}
		}
	}

	// 3f. Adding: a value the function already outputs (same node, pin and type) reuses that output
	void FRun::ReuseExistingOutputs()
	{
		if (!Existing)
		{
			return;
		}
		for (FOutput& Output : Outputs)
		{
			if (UMaterialExpression* const* InFunction = AlreadyInFunction.Find(Output.Source))
			{
				Output.ExistingOutput = Existing->FindOutput(*InFunction, Output.SourcePin, Output.bMaskRG, Output.Type);
			}
			if (Output.ExistingOutput)
			{
				Output.Name = StripSuffix(Output.ExistingOutput->OutputName.ToString());
				++ReusedOutputs;
			}
		}
	}

	// 4. Where the function goes: a new asset (ask where to save it), or the one being added to
	bool FRun::PrepareFunction()
	{
		if (bAdd)
		{
			AssetName = Function->GetName();
			return true;
		}
		const TArray<UObject*>* EditingObjects = Editor->GetObjectsCurrentlyBeingEdited();
		const UObject* OriginalMaterial = EditingObjects && EditingObjects->Num() > 0 ? (*EditingObjects)[0] : Material;
		FString PackagePath;
		if (!AskForFunctionPath(OriginalMaterial, PackagePath, AssetName))
		{
			return false;	// cancelled
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		Function = Cast<UMaterialFunction>(AssetTools.CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), NewObject<UMaterialFunctionFactoryNew>()));
		if (!Function)
		{
			Fail(LOCTEXT("CreateFailed", "couldn't create the material function asset."));
			return false;
		}
		Function->Description = FString::Printf(TEXT("Parameter data moved out of %s %s."), *OriginalMaterial->GetName(), MadeByTool);
		return true;
	}

	// 5a. Copies keep their layout, shifted so the right-most node sits left of the outputs (and, when adding, below
	//     what the function already has)
	void FRun::CopyMovedNodes()
	{
		int32 MaxX = MIN_int32;
		for (const UMaterialExpression* E : Moved)
		{
			MinX = FMath::Min(MinX, E->MaterialExpressionEditorX);
			MinY = FMath::Min(MinY, E->MaterialExpressionEditorY);
			MaxX = FMath::Max(MaxX, E->MaterialExpressionEditorX);
		}
		OutputX = Existing ? Existing->OutputX : 0;
		const int32 ShiftX = OutputX - 400 - MaxX;
		const int32 ShiftY = (Existing ? Existing->BottomY + 250 : 0) - MinY;

		for (UMaterialExpression* E : Moved)
		{
			if (CanonicalOf[E] != E)
			{
				continue;
			}
			if (UMaterialExpression* const* InFunction = AlreadyInFunction.Find(E))
			{
				CopyOf.Add(E, *InFunction);
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
	}

	// 5b. Relink the copies: links between moved nodes point at the copies, links from outside become inputs
	void FRun::LinkFunctionInputs()
	{
		TArray<UMaterialExpressionFunctionInput*> FunctionInputs;	// parallel to OutsideInputs
		for (UMaterialExpression* E : Moved)
		{
			UMaterialExpression* const* CopyPtr = CopyOf.Find(E);
			if (!CopyPtr || AlreadyInFunction.Contains(E))
			{
				continue;	// duplicates, and the function's own nodes (already linked inside it)
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
					const FString Owner = IsPlainSampler(E) || Copy->HasAParameterName()
						? ParameterNameOf(E)
						: Copy->GetClass()->GetName().Replace(TEXT("MaterialExpression"), TEXT(""));
					FOutsideInput& Outside = OutsideInputs.AddDefaulted_GetRef();
					Outside.Name = UniqueName(FString::Printf(TEXT("%s %s"), *Owner, *Copy->GetInputName(It.Index).ToString()));
					Outside.Source = *It.Input;

					UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(
						UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionInput::StaticClass(), Copy->MaterialExpressionEditorX - 350, Copy->MaterialExpressionEditorY));
					FunctionInput->InputName = FName(Outside.Name);
					FunctionInput->InputType = InputTypeFor(Outside.Source, Copy, It.Index);
					FunctionInput->SortPriority = (Existing ? Existing->MaxInputPriority + 1 : 0) + OutsideInputs.Num() - 1;
					FunctionInput->Description = TEXT("Wired from the material by Create Parameter Data Function.");
					// Other materials already calling the function don't wire the new input; they don't read the new
					// outputs either, so a default (the preview value) keeps them compiling
					FunctionInput->bUsePreviewValueAsDefault = bAdd;
					FunctionInputs.Add(FunctionInput);
					Index = OutsideInputs.Num() - 1;
				}
				// The call's input applies the original mask, so the copy reads the function input whole
				It.Input->Expression = FunctionInputs[Index];
				It.Input->OutputIndex = 0;
				It.Input->SetMask(0, 0, 0, 0, 0);
			}
		}
	}

	// 5c. Outputs "<Name> (Type)", top to bottom like their sources; when adding, after the existing ones. Then save.
	void FRun::AddFunctionOutputs()
	{
		Outputs.StableSort([](const FOutput& A, const FOutput& B) { return A.Source->MaterialExpressionEditorY < B.Source->MaterialExpressionEditorY; });
		TMap<UMaterialExpression*, int32> OutputsPerSource;	// stack a node's outputs (e.g. R, G, A) instead of overlapping them
		int32 NextPriority = Existing ? Existing->MaxOutputPriority + 1 : 0;
		int32 NextReusedY = Existing ? Existing->BottomY + 250 : 0;	// new outputs of a node the function already had
		for (FOutput& Output : Outputs)
		{
			if (Output.ExistingOutput)
			{
				continue;
			}
			UMaterialExpression* Copy = CopyOf[Output.Source];
			int32 OutputY = Copy->MaterialExpressionEditorY + OutputsPerSource.FindOrAdd(Output.Source)++ * 90;
			if (AlreadyInFunction.Contains(Output.Source))
			{
				OutputY = NextReusedY;	// its node sits up with the old outputs: keep the new one clear of them
				NextReusedY += 90;
			}
			UMaterialExpression* From = Copy;
			int32 FromPin = Output.SourcePin;
			if (Output.bMaskRG)
			{
				UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionComponentMask::StaticClass(), OutputX - 200, OutputY));
				Mask->R = true;
				Mask->G = true;
				Mask->B = false;
				Mask->A = false;
				Mask->Input.Connect(Output.SourcePin, Copy);
				From = Mask;
				FromPin = 0;
			}
			UMaterialExpressionFunctionOutput* FunctionOutput = Cast<UMaterialExpressionFunctionOutput>(
				UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionOutput::StaticClass(), OutputX, OutputY));
			FunctionOutput->OutputName = FName(FString::Printf(TEXT("%s %s"), *Output.Name, Suffix(Output.Type)));
			FunctionOutput->Description = Output.Source->Desc;
			FunctionOutput->SortPriority = NextPriority++;
			FunctionOutput->A.Connect(FromPin, From);
		}

		UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		Function->MarkPackageDirty();
		UEditorLoadingAndSavingUtils::SavePackages({ Function->GetOutermost() }, false);
	}

	// 6. Rewire the material (the editor's working copy), as one undo step
	bool FRun::RewireMaterial()
	{
		const FScopedTransaction Transaction(Tool);
		Material->Modify();
		if (!PlaceCall())
		{
			return false;
		}
		AddDeclarations();
		AddUsages();

		// 6d. The moved nodes now live in the function
		for (UMaterialExpression* E : Moved)
		{
			E->Modify();
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, E);
		}
		return true;
	}

	// 6a. The call: a new one where the moved nodes were, or the target call refreshed so it has the new pins (existing
	//     links follow their pins by id). Its new inputs read what the moved nodes used to read.
	bool FRun::PlaceCall()
	{
		if (bAdd)
		{
			Call = TargetCall;
			Call->Modify();
			Call->UpdateFromFunctionResource();
		}
		else
		{
			Call = Cast<UMaterialExpressionMaterialFunctionCall>(
				UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, nullptr, UMaterialExpressionMaterialFunctionCall::StaticClass(), Function, MinX, MinY, false));
			if (!Call || Call->MaterialFunction != Function)
			{
				Fail(LOCTEXT("CallFailed", "couldn't place the function in the material. The function asset was created."));
				return false;
			}
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
		return true;
	}

	// 6b. One named reroute per output. When adding: a reused output keeps its reroute if it has one, and new reroutes
	//     go under the call's existing ones.
	void FRun::AddDeclarations()
	{
		TArray<UMaterialExpressionNamedRerouteDeclaration*> CallDeclarations;
		for (UMaterialExpression* E : Material->GetExpressions())
		{
			UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(E);
			if (Declaration && Declaration->Input.Expression == Call)
			{
				CallDeclarations.Add(Declaration);
			}
		}
		constexpr int32 DeclarationOffset = 450;	// right of the call (function calls are wide)
		constexpr int32 DeclarationStep = 64;
		int32 DeclarationX = MinX + DeclarationOffset;
		int32 DeclarationY = MinY;
		if (bAdd)
		{
			DeclarationX = Call->MaterialExpressionEditorX + DeclarationOffset;
			DeclarationY = Call->MaterialExpressionEditorY;
			for (const UMaterialExpressionNamedRerouteDeclaration* Declaration : CallDeclarations)
			{
				DeclarationX = Declaration->MaterialExpressionEditorX;
				DeclarationY = FMath::Max(DeclarationY, Declaration->MaterialExpressionEditorY + DeclarationStep);
			}
		}
		for (FOutput& Output : Outputs)
		{
			const FName OutputName(FString::Printf(TEXT("%s %s"), *Output.Name, Suffix(Output.Type)));
			const int32 CallPin = Call->GetOutputs().IndexOfByPredicate([&OutputName](const FExpressionOutput& O) { return O.OutputName == OutputName; });
			if (CallPin == INDEX_NONE)
			{
				continue;
			}
			if (UMaterialExpressionNamedRerouteDeclaration** Found = CallDeclarations.FindByPredicate(
				[CallPin](const UMaterialExpressionNamedRerouteDeclaration* D) { return D->Input.OutputIndex == CallPin; }))
			{
				Output.Declaration = *Found;
				continue;
			}
			// Booleans and textures by value, everything else by the output's type
			VrrReroute::EKind Kind = VrrReroute::KindOf(Output.Source, Output.SourcePin);
			if (Kind != VrrReroute::EKind::Bool && Kind != VrrReroute::EKind::Texture)
			{
				Kind = ToKind(Output.Type);
			}
			Output.Declaration = VrrGraph::CreateDeclaration(Material, Call, CallPin, DeclarationX, DeclarationY,
				FString::Printf(TEXT("%s%s"), VrrReroute::Prefix(Kind), *Output.Name), VrrReroute::ColorFor(Output.Source, Output.SourcePin, Kind));
			DeclarationY += DeclarationStep;
		}
	}

	// 6c. A usage next to every outside read. Reads keep their channel mask, except single-channel scalars.
	void FRun::AddUsages()
	{
		TMap<UMaterialExpression*, int32> UsagesPerConsumer;
		for (const FOutput& Output : Outputs)
		{
			if (!Output.Declaration)
			{
				continue;
			}
			for (const FLink& Read : Output.Reads)
			{
				const int32 Slot = UsagesPerConsumer.FindOrAdd(Read.Consumer)++;
				UMaterialExpressionNamedRerouteUsage* Usage = VrrGraph::CreateUsage(Material, Output.Declaration, VrrGraph::UsagePositionFor(Material, Read.Consumer, Slot));
				if (!Usage)
				{
					continue;
				}
				VrrGraph::Rewire(Read, Usage);
				if (Output.bClearReadMasks)
				{
					Read.Input->SetMask(0, 0, 0, 0, 0);
				}
			}
		}
	}

	// 7. Show the new nodes, mark the material as changed (Apply / Save as usual) and report
	void FRun::Finish()
	{
		// 7a. Select the call and its named reroute declarations (not the usages), so they can be moved as one block
		TArray<const UMaterialExpression*> ToSelect = { Call };
		for (UMaterialExpression* E : Material->GetExpressions())
		{
			const UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(E);
			if (Declaration && Declaration->Input.Expression == Call)
			{
				ToSelect.Add(Declaration);
			}
		}
		VrrGraph::FinishEdit(Graph, *Editor, ToSelect);

		FText Message = bAdd
			? FText::Format(LOCTEXT("DoneAdd", "Added {0} node(s) to {1}: {2} new output(s), {3} existing output(s) reused, {4} new input(s)."),
				Moved.Num(), FText::FromString(AssetName), Outputs.Num() - ReusedOutputs, ReusedOutputs, OutsideInputs.Num())
			: FText::Format(LOCTEXT("Done", "Moved {0} node(s) into {1}: {2} output(s), {3} input(s)."),
				Moved.Num(), FText::FromString(AssetName), Outputs.Num(), OutsideInputs.Num());
		if (!Skipped.IsEmpty())
		{
			Message = FText::Format(LOCTEXT("DoneSkipped", "{0}\nSkipped: {1}"), Message, FText::FromString(FString::Join(Skipped, TEXT(", "))));
		}
		VrrGraph::Notify(Message, true);
	}

	void CreateFromSelection(UMaterialGraph* Graph)
	{
		FRun(Graph, nullptr).Execute();
	}

	void AddToFunction(UMaterialGraph* Graph, UMaterialExpressionMaterialFunctionCall* Call)
	{
		if (Call)
		{
			FRun(Graph, Call).Execute();
		}
	}
}

#undef LOCTEXT_NAMESPACE
