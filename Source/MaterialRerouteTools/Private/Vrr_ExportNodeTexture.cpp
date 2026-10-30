// Copyright VR Robotica. All Rights Reserved.

/*
 * Export Node to Texture (see Vrr_ExportNodeTexture.h).
 *
 * Steps:
 *   1. Options: size and format (remembered per project in EditorPerProjectUserSettings).
 *   2. Where to save: the Content Browser's save dialog, next to the material by default. Picking an existing texture
 *      updates it in place, so a node can be re-exported after the graph changes.
 *   3. Bake: a transient copy of the material being edited (with unapplied edits) is turned into an opaque Unlit
 *      surface material whose only wired output is the node's value, compiled, and drawn into an RGBA32f render
 *      target. A 4-channel value takes a second pass with a component mask for its alpha.
 *      In a material function, the function is copied instead, the copy gets an extra output fed by the node, and a
 *      new Unlit material calls it. Function inputs then behave as in any call: only those with "Use Preview Value
 *      as Default" have a value, so the tool refuses nodes that read any other input.
 *   4. Write: the pixels become the texture's source data (sRGB 8-bit, linear 8-bit or 16-bit float).
 */

#include "Vrr_ExportNodeTexture.h"

#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "IMaterialEditor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Algo/Transform.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "RenderUtils.h"
#include "ShaderCompiler.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "VrrExportNodeTexture"

namespace VrrExportNode
{
	namespace
	{
		enum class EFormat : uint8
		{
			Color,	// 8-bit sRGB: colors
			Linear,	// 8-bit linear: masks and other 0-1 data
			HDR		// 16-bit float: values outside 0-1
		};

		struct FOptions
		{
			int32 Size = 1024;
			EFormat Format = EFormat::Color;
		};

		const TCHAR* ConfigSection = TEXT("MaterialRerouteTools.ExportNodeTexture");
		const int32 Sizes[] = { 128, 256, 512, 1024, 2048, 4096 };

		/** Value types a texture can't hold. */
		constexpr uint64 NotAValue = static_cast<uint64>(MCT_Texture) | MCT_SparseVolumeTexture | MCT_StaticBool | MCT_Bool | MCT_MaterialAttributes
			| MCT_ShadingModel | MCT_Substrate | MCT_Execution | MCT_VoidStatement | MCT_VTPageTableResult | MCT_TextureCollection;

		UMaterialExpression* ExpressionOf(const UEdGraphNode* Node)
		{
			const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
			return MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
		}

		/** Channels the output gives: 1-4 (an unknown float counts as 3, like a color). */
		int32 WidthOf(UMaterialExpression* Expression, int32 Output)
		{
			switch (VrrReroute::InferKind(Expression, Output))
			{
			case VrrReroute::EKind::Scalar:  return 1;
			case VrrReroute::EKind::Vector2: return 2;
			case VrrReroute::EKind::Vector4: return 4;
			default:                         return 3;
			}
		}

		FOptions LoadOptions(int32 Width)
		{
			FOptions Options;
			// Colors default to sRGB, single values and UV-like pairs to linear data
			Options.Format = Width >= 3 ? EFormat::Color : EFormat::Linear;
			GConfig->GetInt(ConfigSection, TEXT("Size"), Options.Size, GEditorPerProjectIni);
			int32 Format = 0;
			if (GConfig->GetInt(ConfigSection, Width >= 3 ? TEXT("ColorFormat") : TEXT("DataFormat"), Format, GEditorPerProjectIni))
			{
				Options.Format = static_cast<EFormat>(FMath::Clamp(Format, 0, 2));
			}
			return Options;
		}

		void SaveOptions(const FOptions& Options, int32 Width)
		{
			GConfig->SetInt(ConfigSection, TEXT("Size"), Options.Size, GEditorPerProjectIni);
			GConfig->SetInt(ConfigSection, Width >= 3 ? TEXT("ColorFormat") : TEXT("DataFormat"), static_cast<int32>(Options.Format), GEditorPerProjectIni);
		}

		FText SizeText(int32 Size)
		{
			const FText Number = FText::AsNumber(Size, &FNumberFormattingOptions::DefaultNoGrouping());
			return FText::Format(LOCTEXT("SizeItem", "{0} x {0}"), Number);
		}

		/** The options dialog. Returns false if cancelled. */
		bool AskOptions(const FText& What, int32 Width, FOptions& InOutOptions)
		{
			TSharedRef<FOptions> State = MakeShared<FOptions>(InOutOptions);
			TSharedRef<bool> bExport = MakeShared<bool>(false);

			TSharedRef<TArray<TSharedPtr<int32>>> SizeItems = MakeShared<TArray<TSharedPtr<int32>>>();
			TSharedPtr<int32> InitialSize;
			for (const int32 Size : Sizes)
			{
				SizeItems->Add(MakeShared<int32>(Size));
				if (Size == State->Size)
				{
					InitialSize = SizeItems->Last();
				}
			}
			if (!InitialSize)
			{
				State->Size = 1024;
				InitialSize = (*SizeItems)[3];
			}

			TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(LOCTEXT("WindowTitle", "Export Node to Texture"))
				.SizingRule(ESizingRule::Autosized)
				.SupportsMinimize(false)
				.SupportsMaximize(false);

			auto FormatOption = [State](EFormat Format, const FText& Label, const FText& ToolTip) -> TSharedRef<SWidget>
			{
				return SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.ToolTipText(ToolTip)
					.IsChecked_Lambda([State, Format]() { return State->Format == Format ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State, Format](ECheckBoxState) { State->Format = Format; })
					[
						SNew(STextBlock).Text(Label)
					];
			};

			const FText ChannelsText = Width == 4
				? LOCTEXT("ChannelsRGBA", "RGBA: all four channels, alpha included.")
				: Width == 1
				? LOCTEXT("ChannelsScalar", "A single value: stored as a grayscale texture.")
				: Width == 2
				? LOCTEXT("ChannelsRG", "Two channels: stored in R and G (B is 0).")
				: LOCTEXT("ChannelsRGB", "RGB.");

			Window->SetContent(
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(16.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(What).Font(FAppStyle::GetFontStyle("BoldFont"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(STextBlock).Text(ChannelsText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							SNew(STextBlock).Text(LOCTEXT("SizeLabel", "Size"))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SComboBox<TSharedPtr<int32>>)
							.OptionsSource(&SizeItems.Get())
							.InitiallySelectedItem(InitialSize)
							.OnGenerateWidget_Lambda([](TSharedPtr<int32> Item) { return SNew(STextBlock).Text(SizeText(*Item)); })
							.OnSelectionChanged_Lambda([State](TSharedPtr<int32> Item, ESelectInfo::Type) { if (Item) { State->Size = *Item; } })
							[
								SNew(STextBlock).Text_Lambda([State]() { return SizeText(State->Size); })
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("FormatLabel", "Format"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						FormatOption(EFormat::Color, LOCTEXT("FormatColor", "Color (sRGB, 8-bit)"),
							LOCTEXT("FormatColorTip", "For colors. Stored as sRGB, so sampling it gives the node's values back. Values are clamped to 0-1."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						FormatOption(EFormat::Linear, LOCTEXT("FormatLinear", "Linear data (8-bit)"),
							LOCTEXT("FormatLinearTip", "For masks, gradients, noise and other 0-1 data. sRGB off. Values are clamped to 0-1."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 12.0f)
					[
						FormatOption(EFormat::HDR, LOCTEXT("FormatHDR", "HDR (16-bit float)"),
							LOCTEXT("FormatHDRTip", "Keeps values below 0 and above 1 (offsets, heights, emissive). Larger on disk."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(STextBlock)
						.WrapTextAt(420.0f)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT("ExportNote",
							"The node is rendered across the texture's 0-1 UVs, using parameter defaults. Nodes that read the scene "
							"(world position, camera, scene textures, vertex data) have nothing to read here."))
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SUniformGridPanel).SlotPadding(FMargin(4.0f, 0.0f))
						+ SUniformGridPanel::Slot(0, 0)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT("ExportButton", "Export..."))
							.OnClicked_Lambda([bExport, WeakWindow = TWeakPtr<SWindow>(Window)]()
							{
								*bExport = true;
								if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
								return FReply::Handled();
							})
						]
						+ SUniformGridPanel::Slot(1, 0)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT("CancelButton", "Cancel"))
							.OnClicked_Lambda([WeakWindow = TWeakPtr<SWindow>(Window)]()
							{
								if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
								return FReply::Handled();
							})
						]
					]
				]);

			FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
			if (*bExport)
			{
				InOutOptions = *State;
			}
			return *bExport;
		}

		/** Letters, digits and single underscores. */
		FString CleanName(const FString& In)
		{
			FString Out;
			for (const TCHAR C : In)
			{
				if (FChar::IsAlnum(C))
				{
					Out.AppendChar(C);
				}
				else if (!Out.IsEmpty() && !Out.EndsWith(TEXT("_")))
				{
					Out.AppendChar(TEXT('_'));
				}
			}
			Out.RemoveFromEnd(TEXT("_"));
			return Out;
		}

		/** Asks where to save the texture. Returns false if cancelled. */
		bool AskForTexturePath(const UObject* OriginalMaterial, const FString& DefaultName, FString& OutObjectPath)
		{
			const FString MaterialPackage = OriginalMaterial->GetOutermost()->GetName();

			FSaveAssetDialogConfig Config;
			Config.DialogTitleOverride = LOCTEXT("SaveTitle", "Save Node Texture");
			Config.DefaultPath = FPackageName::GetLongPackagePath(MaterialPackage);
			Config.DefaultAssetName = DefaultName;
			Config.AssetClassNames.Add(UTexture2D::StaticClass()->GetClassPathName());
			Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;	// re-exporting updates the texture

			IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
			OutObjectPath = ContentBrowser.CreateModalSaveAssetDialog(Config);
			return !OutObjectPath.IsEmpty();
		}

		/** The copy of Node among Expressions (copies keep their names), or null. */
		template <typename TExpressions>
		UMaterialExpression* FindCopy(const TExpressions& Expressions, const UMaterialExpression* Node)
		{
			for (UMaterialExpression* E : Expressions)
			{
				if (E && E->GetFName() == Node->GetFName())
				{
					return E;
				}
			}
			return nullptr;
		}

		UObject* KeepTransient(UObject* Object)
		{
			Object->SetFlags(RF_Transient);
			Object->AddToRoot();	// nothing else references it while the progress dialog lets the editor tick
			return Object;
		}

		/** The material the node is baked from, with its value wired to an Unlit output. */
		class FBake
		{
		public:
			/** Node is in a material: bake from a copy of the material being edited. */
			bool InitFromMaterial(UMaterial* EditedMaterial, const UMaterialExpression* Node, int32 InOutput, FText& OutError)
			{
				Material = DuplicateObject<UMaterial>(EditedMaterial, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("VrrNodeBake")));
				KeepTransient(Material);
				Expression = FindCopy(Material->GetExpressions(), Node);
				Output = InOutput;
				if (!Expression)
				{
					OutError = LOCTEXT("NodeNotFound", "Couldn't find the node in a copy of the material.");
					return false;
				}
				return MakeUnlit();
			}

			/**
			 * Node is in a material function: copy the function, give the copy an extra output fed by the node, and
			 * call it from a new material. Called like any function, its inputs use their preview values only when
			 * "Use Preview Value as Default" is on (see FunctionInputsWithoutDefault).
			 */
			bool InitFromFunction(UMaterialFunction* EditedFunction, const UMaterialExpression* Node, int32 InOutput, FText& OutError)
			{
				Function = DuplicateObject<UMaterialFunction>(EditedFunction, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterialFunction::StaticClass(), TEXT("VrrNodeBakeFunction")));
				KeepTransient(Function);
				UMaterialExpression* Copy = FindCopy(Function->GetExpressions(), Node);
				if (!Copy)
				{
					OutError = LOCTEXT("FunctionNodeNotFound", "Couldn't find the node in a copy of the function.");
					return false;
				}
				UMaterialExpressionFunctionOutput* BakeOutput = Cast<UMaterialExpressionFunctionOutput>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionOutput::StaticClass()));
				BakeOutput->OutputName = TEXT("VrrBakeOutput");
				BakeOutput->SortPriority = TNumericLimits<int32>::Max();
				BakeOutput->A.Connect(InOutput, Copy);

				Material = NewObject<UMaterial>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("VrrNodeBake")));
				KeepTransient(Material);
				UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
					UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass()));
				Call->SetMaterialFunction(Function);
				for (int32 Index = 0; Index < Call->FunctionOutputs.Num(); ++Index)
				{
					if (Call->FunctionOutputs[Index].ExpressionOutput == BakeOutput)
					{
						Expression = Call;
						Output = Index;
					}
				}
				if (!Expression)
				{
					OutError = LOCTEXT("FunctionCallFailed", "Couldn't call a copy of the function.");
					return false;
				}
				return MakeUnlit();
			}

		private:
			/** An opaque Unlit surface whose only wired output is the value. */
			bool MakeUnlit()
			{
				Material->MaterialDomain = MD_Surface;
				Material->BlendMode = BLEND_Opaque;
				Material->SetShadingModel(MSM_Unlit);
				Material->bUseMaterialAttributes = false;
				for (int32 Property = 0; Property < MP_MAX; ++Property)
				{
					FMaterialInputDescription Description;
					if (Material->GetExpressionInputDescription(static_cast<EMaterialProperty>(Property), Description) && Description.Input)
					{
						Description.Input->Expression = nullptr;
					}
				}
				if (Substrate::IsSubstrateEnabled())
				{
					Unlit = Cast<UMaterialExpressionSubstrateUnlitBSDF>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSubstrateUnlitBSDF::StaticClass()));
					Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Unlit);
				}
				return true;
			}

		public:
			/** Draws the value (or only its alpha) into Pixels. */
			bool Render(bool bAlpha, int32 Size, TArray<FLinearColor>& Pixels, FText& OutError)
			{
				UMaterialExpression* Source = Expression;
				int32 SourceOutput = Output;
				if (bAlpha)
				{
					if (!AlphaMask)
					{
						AlphaMask = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComponentMask::StaticClass()));
						AlphaMask->R = AlphaMask->G = AlphaMask->B = false;
						AlphaMask->A = true;
						AlphaMask->Input.Connect(Output, Expression);
					}
					Source = AlphaMask;
					SourceOutput = 0;
				}
				FExpressionInput& Emissive = Unlit ? Unlit->EmissiveColor : Material->GetEditorOnlyData()->EmissiveColor;
				Emissive.Connect(SourceOutput, Source);

				UMaterialEditingLibrary::RecompileMaterial(Material);
				if (GShaderCompilingManager)
				{
					GShaderCompilingManager->FinishAllCompilation();
				}
#if UE_VERSION_OLDER_THAN(5, 7, 0)
				const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel);
#else
				const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
#endif
				if (!Resource || !Resource->GetCompileErrors().IsEmpty())
				{
					const FString Errors = Resource ? FString::Join(Resource->GetCompileErrors(), TEXT("\n")) : FString(TEXT("no shaders"));
					OutError = FText::Format(LOCTEXT("CompileFailed", "The node doesn't compile on its own as an Unlit surface:\n{0}"), FText::FromString(Errors));
					return false;
				}

				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				UTextureRenderTarget2D* Target = UKismetRenderingLibrary::CreateRenderTarget2D(World, Size, Size, RTF_RGBA32f, FLinearColor::Black, false);
				if (!Target)
				{
					OutError = LOCTEXT("NoTarget", "Couldn't create the render target.");
					return false;
				}
				UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Target, Material);
				FTextureRenderTargetResource* TargetResource = Target->GameThread_GetRenderTargetResource();
				const bool bRead = TargetResource && TargetResource->ReadLinearColorPixels(Pixels);	// RCM_MinMax: values outside 0-1 kept
				Target->MarkAsGarbage();
				if (!bRead || Pixels.Num() != Size * Size)
				{
					OutError = LOCTEXT("ReadFailed", "Couldn't read the rendered pixels back.");
					return false;
				}
				return true;
			}

			~FBake()
			{
				for (UObject* Object : { static_cast<UObject*>(Material), static_cast<UObject*>(Function) })
				{
					if (Object)
					{
						Object->RemoveFromRoot();
						Object->MarkAsGarbage();
					}
				}
			}

		private:
			UMaterial* Material = nullptr;
			UMaterialFunction* Function = nullptr;
			UMaterialExpression* Expression = nullptr;
			UMaterialExpressionSubstrateUnlitBSDF* Unlit = nullptr;
			UMaterialExpressionComponentMask* AlphaMask = nullptr;
			int32 Output = 0;
		};

		/** Writes Pixels into the texture at ObjectPath (created, or updated if it's already a texture). */
		UTexture2D* WriteTexture(const FString& ObjectPath, int32 Size, int32 Width, EFormat Format, const TArray<FLinearColor>& Pixels, FText& OutError)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			const FString AssetName = FPackageName::GetShortName(PackageName);

			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const bool bNew = Texture == nullptr;
			if (bNew)
			{
				if (FindObject<UObject>(nullptr, *ObjectPath) || FPackageName::DoesPackageExist(PackageName))
				{
					OutError = FText::Format(LOCTEXT("NotATexture", "{0} already exists and isn't a texture."), FText::FromString(AssetName));
					return nullptr;
				}
				UPackage* Package = CreatePackage(*PackageName);
				Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			}
			else
			{
				Texture->Modify();
			}

			Texture->PreEditChange(nullptr);
			if (Format == EFormat::HDR)
			{
				TArray<FFloat16Color> Data;
				Data.Reserve(Pixels.Num());
				for (const FLinearColor& P : Pixels)
				{
					Data.Add(FFloat16Color(P));
				}
				Texture->Source.Init(Size, Size, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(Data.GetData()));
				Texture->SRGB = false;
				Texture->CompressionSettings = Width == 1 ? TC_HalfFloat : TC_HDR;
			}
			else
			{
				const bool bSRGB = Format == EFormat::Color;
				TArray<FColor> Data;
				Data.Reserve(Pixels.Num());
				for (const FLinearColor& P : Pixels)
				{
					Data.Add(P.ToFColor(bSRGB));	// clamps to 0-1
				}
				Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Data.GetData()));
				Texture->SRGB = bSRGB;
				Texture->CompressionSettings = bSRGB ? TC_Default : Width == 1 ? TC_Grayscale : TC_Masks;
			}
			Texture->CompressionNoAlpha = Width < 4;
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
			if (bNew)
			{
				FAssetRegistryModule::AssetCreated(Texture);
			}
			return Texture;
		}

		/** Smallest and largest value over the channels the output has. */
		void ValueRange(const TArray<FLinearColor>& Pixels, int32 Width, float& OutMin, float& OutMax)
		{
			OutMin = TNumericLimits<float>::Max();
			OutMax = TNumericLimits<float>::Lowest();
			for (const FLinearColor& P : Pixels)
			{
				const float Channels[4] = { P.R, P.G, P.B, P.A };
				for (int32 C = 0; C < FMath::Min(Width, 4); ++C)
				{
					OutMin = FMath::Min(OutMin, Channels[C]);
					OutMax = FMath::Max(OutMax, Channels[C]);
				}
			}
		}
	}

	TArray<FOutput> GetExportableOutputs(const UEdGraphNode* Node)
	{
		TArray<FOutput> Result;
		UMaterialExpression* Expression = ExpressionOf(Node);
		if (!Expression)
		{
			return Result;
		}
		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		for (int32 Index = 0; Index < Outputs.Num(); ++Index)
		{
			if ((static_cast<uint64>(Expression->GetOutputValueType(Index)) & NotAValue) != 0)
			{
				continue;
			}
			const VrrReroute::EKind Kind = VrrReroute::InferKind(Expression, Index);
			if (Kind == VrrReroute::EKind::Bool || Kind == VrrReroute::EKind::Texture || Kind == VrrReroute::EKind::MaterialAttributes || Kind == VrrReroute::EKind::ShadingModel)
			{
				continue;
			}
			FText Label = FText::FromName(Outputs[Index].OutputName);
			if (Outputs[Index].OutputName.IsNone())
			{
				Label = Outputs.Num() == 1 ? Node->GetNodeTitle(ENodeTitleType::ListView) : FText::Format(LOCTEXT("OutputN", "Output {0}"), Index);
			}
			Result.Add({ Index, Label });
		}
		return Result;
	}

	TArray<FName> FunctionInputsWithoutDefault(const UEdGraphNode* Node)
	{
		TArray<FName> Missing;
		TSet<UMaterialExpression*> Seen;
		TArray<UMaterialExpression*> ToVisit;
		if (UMaterialExpression* Start = ExpressionOf(Node))
		{
			ToVisit.Add(Start);
		}
		while (!ToVisit.IsEmpty())
		{
			UMaterialExpression* E = ToVisit.Pop(EAllowShrinking::No);
			if (!E || Seen.Contains(E))
			{
				continue;
			}
			Seen.Add(E);
			if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(E))
			{
				if (!Input->bUsePreviewValueAsDefault)
				{
					Missing.AddUnique(Input->InputName);
					continue;	// its preview graph won't be used
				}
			}
			if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E))
			{
				ToVisit.Add(Usage->Declaration);
			}
			for (FExpressionInputIterator It{ E }; It; ++It)
			{
				ToVisit.Add(It.Input->Expression);
			}
		}
		return Missing;
	}

	int32 OutputIndexOfPin(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
		if (!MaterialNode || !Pin || Pin->Direction != EGPD_Output || !MaterialNode->MaterialExpression)
		{
			return INDEX_NONE;
		}
		for (int32 Index = 0; Index < MaterialNode->MaterialExpression->GetOutputs().Num(); ++Index)
		{
			if (MaterialNode->GetOutputPin(Index) == Pin)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	void Export(UMaterialGraph* Graph, const UEdGraphNode* Node, int32 Output)
	{
		UMaterialExpression* Expression = ExpressionOf(Node);
		TSharedPtr<IMaterialEditor> Editor = VrrGraph::GetEditor(Graph);
		UMaterialFunction* EditedFunction = Graph ? Graph->MaterialFunction.Get() : nullptr;
		if (!Graph || (!Graph->Material && !EditedFunction) || !Expression || !Editor)
		{
			return;
		}
		if (EditedFunction)
		{
			const TArray<FName> Missing = FunctionInputsWithoutDefault(Node);
			if (!Missing.IsEmpty())
			{
				TArray<FString> Names;
				Algo::Transform(Missing, Names, [](FName N) { return N.ToString(); });
				VrrGraph::Notify(FText::Format(LOCTEXT("NoDefault", "Export Node to Texture: turn on \"Use Preview Value as Default\" on function input(s) {0}, which this node reads."),
					FText::FromString(FString::Join(Names, TEXT(", ")))), false);
				return;
			}
		}
		const TArray<FOutput> Outputs = GetExportableOutputs(Node);
		const FOutput* Chosen = Outputs.FindByPredicate([Output](const FOutput& O) { return O.Index == Output; });
		if (!Chosen)
		{
			return;
		}
		const int32 Width = WidthOf(Expression, Output);
		const FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
		const FText What = Chosen->Label.EqualTo(NodeTitle) ? NodeTitle : FText::Format(LOCTEXT("NodeAndOutput", "{0}: {1}"), NodeTitle, Chosen->Label);

		// 1. Options
		FOptions Options = LoadOptions(Width);
		if (!AskOptions(What, Width, Options))
		{
			return;
		}
		SaveOptions(Options, Width);

		// 2. Where: T_<Material or function>_<Node>[_<Output>], without its "M_" / "MF_"
		const TArray<UObject*>* EditingObjects = Editor->GetObjectsCurrentlyBeingEdited();
		const UObject* OriginalMaterial = EditingObjects && EditingObjects->Num() > 0 ? (*EditingObjects)[0] : Graph->GetMaterialOrFunction();
		FString MaterialName = OriginalMaterial->GetName();
		MaterialName.RemoveFromStart(TEXT("MF_"));
		MaterialName.RemoveFromStart(TEXT("M_"));
		FString DefaultName = TEXT("T_") + CleanName(MaterialName) + TEXT("_") + CleanName(NodeTitle.ToString().Left(32));
		if (!Chosen->Label.EqualTo(NodeTitle))
		{
			DefaultName += TEXT("_") + CleanName(Chosen->Label.ToString());
		}
		FString ObjectPath;
		if (!AskForTexturePath(OriginalMaterial, DefaultName, ObjectPath))
		{
			return;
		}

		// 3. Bake
		FScopedSlowTask Task(Width == 4 ? 3.0f : 2.0f, LOCTEXT("Baking", "Exporting node to texture..."));
		Task.MakeDialog();
		FText Error;
		FBake Bake;
		TArray<FLinearColor> Pixels;
		Task.EnterProgressFrame(1.0f, LOCTEXT("BakingColor", "Compiling and rendering the node..."));
		const bool bReady = EditedFunction
			? Bake.InitFromFunction(EditedFunction, Expression, Output, Error)
			: Bake.InitFromMaterial(Graph->Material, Expression, Output, Error);
		bool bOk = bReady && Bake.Render(false, Options.Size, Pixels, Error);
		if (bOk && Width == 4)
		{
			Task.EnterProgressFrame(1.0f, LOCTEXT("BakingAlpha", "Rendering alpha..."));
			TArray<FLinearColor> Alpha;
			bOk = Bake.Render(true, Options.Size, Alpha, Error);
			for (int32 I = 0; bOk && I < Pixels.Num(); ++I)
			{
				Pixels[I].A = Alpha[I].R;
			}
		}
		else
		{
			for (FLinearColor& P : Pixels)
			{
				P.A = 1.0f;
			}
		}
		if (bOk && Width == 1)
		{
			for (FLinearColor& P : Pixels)
			{
				P.G = P.B = P.R;
			}
		}
		else if (bOk && Width == 2)
		{
			for (FLinearColor& P : Pixels)
			{
				P.B = 0.0f;
			}
		}

		// 4. Write
		UTexture2D* Texture = nullptr;
		if (bOk)
		{
			Task.EnterProgressFrame(1.0f, LOCTEXT("Writing", "Writing the texture..."));
			Texture = WriteTexture(ObjectPath, Options.Size, Width, Options.Format, Pixels, Error);
		}
		if (!Texture)
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("ExportFailed", "Export Node to Texture failed: {0}"), Error), false);
			return;
		}

		IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
		ContentBrowser.SyncBrowserToAssets(TArray<UObject*>{ Texture });

		float Min = 0.0f, Max = 0.0f;
		ValueRange(Pixels, Width, Min, Max);
		FNumberFormattingOptions Digits;
		Digits.SetMaximumFractionalDigits(3);
		FText Message = FText::Format(LOCTEXT("Exported", "Exported {0} to {1} ({2}). Values range {3} to {4}."),
			What, FText::FromString(Texture->GetName()), SizeText(Options.Size), FText::AsNumber(Min, &Digits), FText::AsNumber(Max, &Digits));
		if (Options.Format != EFormat::HDR && (Min < -0.001f || Max > 1.001f))
		{
			Message = FText::Format(LOCTEXT("Clamped", "{0} Values outside 0-1 were clamped; export as HDR to keep them."), Message);
		}
		VrrGraph::Notify(Message, true);
	}
}

#undef LOCTEXT_NAMESPACE
