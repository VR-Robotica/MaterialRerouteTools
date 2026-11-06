// Copyright VR Robotica. All Rights Reserved.

/*
 * Export Node to Texture (see Vrr_ExportNodeTexture.h).
 *
 * Steps:
 *   1. Options: resolution and format, remembered per project in EditorPerProjectUserSettings.
 *   2. Destination: the Content Browser's save dialog, next to the material by default. Picking an existing texture
 *      updates it in place, so a node can be re-exported after the graph changes.
 *   3. Bake: a transient copy of the material being edited (unapplied edits included) becomes an opaque Unlit surface
 *      whose only wired output is the node's value. It is compiled and drawn into an RGBA32f render target. A
 *      4-channel value takes a second pass through a component mask for its alpha.
 *      For a node in a material function, the function is copied instead, the copy gets an extra output fed by the
 *      node, and a new Unlit material calls it. Function inputs then behave as in any call: only those with "Use
 *      Preview Value as Default" have a value, so the tool refuses nodes that read any other input.
 *      Before drawing, the bake checks its shaders on the editor's highest platform and on the one the editor world
 *      draws with (they differ under a Preview Rendering Level). A material without shaders there is silently drawn
 *      as the default material, whose emissive is black. Sampled textures are finished building and fully streamed
 *      in, so the bake doesn't read placeholders or low mips.
 *   4. Write: the pixels become the texture's source data (sRGB 8-bit, linear 8-bit or 16-bit float). A result that
 *      is 0 in every channel almost always means the bake failed, so the tool asks before saving it.
 */

#include "Vrr_ExportNodeTexture.h"

#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "Algo/Transform.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "IMaterialEditor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Vrr_EngineCompat.h"
#include "Materials/MaterialFunction.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "RenderUtils.h"
#include "Styling/AppStyle.h"
#include "TextureCompiler.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogVrrExportNode, Log, All);

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

		struct FExportOptions
		{
			int32 Resolution = 1024;
			EFormat Format = EFormat::Color;
		};

		/** Smallest and largest value over the channels an output has. */
		struct FValueRange
		{
			float Min = 0.0f;
			float Max = 0.0f;
		};

		const TCHAR* ConfigSection = TEXT("MaterialRerouteTools.ExportNodeTexture");
		const int32 Resolutions[] = { 128, 256, 512, 1024, 2048, 4096 };
		constexpr int32 DefaultResolution = 1024;

		/** Value types a texture can't hold. */
		constexpr uint64 NonValueTypes = static_cast<uint64>(MCT_Texture) | VRR_MCT_SPARSE_VOLUME_TEXTURE | MCT_StaticBool | MCT_Bool | MCT_MaterialAttributes
			| MCT_ShadingModel | MCT_Substrate | MCT_Execution | MCT_VoidStatement | MCT_VTPageTableResult | VRR_MCT_TEXTURE_COLLECTION;

		UMaterialExpression* GetExpression(const UEdGraphNode* Node)
		{
			const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
			return MaterialNode ? MaterialNode->MaterialExpression.Get() : nullptr;
		}

		/** Channels an output gives, 1-4. A float of unknown width counts as 3, like a color. */
		int32 GetChannelCount(UMaterialExpression* Expression, int32 OutputIndex)
		{
			switch (VrrReroute::InferKind(Expression, OutputIndex))
			{
			case VrrReroute::EKind::Scalar:  return 1;
			case VrrReroute::EKind::Vector2: return 2;
			case VrrReroute::EKind::Vector4: return 4;
			default:                         return 3;
			}
		}

		/** Colors and data keep separate format choices, so exporting a mask doesn't change the next color export. */
		const TCHAR* GetFormatConfigKey(int32 ChannelCount)
		{
			return ChannelCount >= 3 ? TEXT("ColorFormat") : TEXT("DataFormat");
		}

		FExportOptions LoadOptions(int32 ChannelCount)
		{
			FExportOptions Options;
			Options.Format = ChannelCount >= 3 ? EFormat::Color : EFormat::Linear;
			GConfig->GetInt(ConfigSection, TEXT("Size"), Options.Resolution, GEditorPerProjectIni);
			int32 SavedFormat = 0;
			if (GConfig->GetInt(ConfigSection, GetFormatConfigKey(ChannelCount), SavedFormat, GEditorPerProjectIni))
			{
				Options.Format = static_cast<EFormat>(FMath::Clamp(SavedFormat, 0, 2));
			}
			return Options;
		}

		void SaveOptions(const FExportOptions& Options, int32 ChannelCount)
		{
			GConfig->SetInt(ConfigSection, TEXT("Size"), Options.Resolution, GEditorPerProjectIni);
			GConfig->SetInt(ConfigSection, GetFormatConfigKey(ChannelCount), static_cast<int32>(Options.Format), GEditorPerProjectIni);
		}

		FText FormatResolution(int32 Resolution)
		{
			const FText Number = FText::AsNumber(Resolution, &FNumberFormattingOptions::DefaultNoGrouping());
			return FText::Format(LOCTEXT("SizeItem", "{0} x {0}"), Number);
		}

		FText DescribeChannels(int32 ChannelCount)
		{
			switch (ChannelCount)
			{
			case 1:  return LOCTEXT("ChannelsScalar", "A single value: stored as a grayscale texture.");
			case 2:  return LOCTEXT("ChannelsRG", "Two channels: stored in R and G (B is 0).");
			case 4:  return LOCTEXT("ChannelsRGBA", "RGBA: all four channels, alpha included.");
			default: return LOCTEXT("ChannelsRGB", "RGB.");
			}
		}

		/** The options dialog. Returns false if cancelled. */
		bool AskExportOptions(const FText& ExportName, int32 ChannelCount, FExportOptions& InOutOptions)
		{
			TSharedRef<FExportOptions> DialogOptions = MakeShared<FExportOptions>(InOutOptions);
			TSharedRef<bool> bConfirmed = MakeShared<bool>(false);

			TSharedRef<TArray<TSharedPtr<int32>>> ResolutionItems = MakeShared<TArray<TSharedPtr<int32>>>();
			TSharedPtr<int32> InitialResolution;
			for (const int32 Resolution : Resolutions)
			{
				ResolutionItems->Add(MakeShared<int32>(Resolution));
				if (Resolution == DialogOptions->Resolution)
				{
					InitialResolution = ResolutionItems->Last();
				}
			}
			if (!InitialResolution)
			{
				DialogOptions->Resolution = DefaultResolution;
				InitialResolution = *ResolutionItems->FindByPredicate([](const TSharedPtr<int32>& Item) { return *Item == DefaultResolution; });
			}

			TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(LOCTEXT("WindowTitle", "Export Node to Texture"))
				.SizingRule(ESizingRule::Autosized)
				.SupportsMinimize(false)
				.SupportsMaximize(false);
			const TWeakPtr<SWindow> WeakWindow = Window;
			auto CloseWindow = [WeakWindow]()
			{
				if (TSharedPtr<SWindow> PinnedWindow = WeakWindow.Pin())
				{
					PinnedWindow->RequestDestroyWindow();
				}
			};

			auto MakeFormatOption = [DialogOptions](EFormat Format, const FText& Label, const FText& ToolTip) -> TSharedRef<SWidget>
			{
				return SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.ToolTipText(ToolTip)
					.IsChecked_Lambda([DialogOptions, Format]() { return DialogOptions->Format == Format ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([DialogOptions, Format](ECheckBoxState) { DialogOptions->Format = Format; })
					[
						SNew(STextBlock).Text(Label)
					];
			};

			Window->SetContent(
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(16.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(ExportName).Font(FAppStyle::GetFontStyle("BoldFont"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(STextBlock).Text(DescribeChannels(ChannelCount))
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
							.OptionsSource(&ResolutionItems.Get())
							.InitiallySelectedItem(InitialResolution)
							.OnGenerateWidget_Lambda([](TSharedPtr<int32> Item) { return SNew(STextBlock).Text(FormatResolution(*Item)); })
							.OnSelectionChanged_Lambda([DialogOptions](TSharedPtr<int32> Item, ESelectInfo::Type)
							{
								if (Item)
								{
									DialogOptions->Resolution = *Item;
								}
							})
							[
								SNew(STextBlock).Text_Lambda([DialogOptions]() { return FormatResolution(DialogOptions->Resolution); })
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("FormatLabel", "Format"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						MakeFormatOption(EFormat::Color, LOCTEXT("FormatColor", "Color (sRGB, 8-bit)"),
							LOCTEXT("FormatColorTip", "For colors. Stored as sRGB, so sampling it gives the node's values back. Values are clamped to 0-1."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						MakeFormatOption(EFormat::Linear, LOCTEXT("FormatLinear", "Linear data (8-bit)"),
							LOCTEXT("FormatLinearTip", "For masks, gradients, noise and other 0-1 data. sRGB off. Values are clamped to 0-1."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 12.0f)
					[
						MakeFormatOption(EFormat::HDR, LOCTEXT("FormatHDR", "HDR (16-bit float)"),
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
							.OnClicked_Lambda([bConfirmed, CloseWindow]()
							{
								*bConfirmed = true;
								CloseWindow();
								return FReply::Handled();
							})
						]
						+ SUniformGridPanel::Slot(1, 0)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.Text(LOCTEXT("CancelButton", "Cancel"))
							.OnClicked_Lambda([CloseWindow]()
							{
								CloseWindow();
								return FReply::Handled();
							})
						]
					]
				]);

			FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
			if (*bConfirmed)
			{
				InOutOptions = *DialogOptions;
			}
			return *bConfirmed;
		}

		/** Letters and digits, with runs of anything else turned into a single underscore. */
		FString ToAssetNamePart(const FString& Text)
		{
			FString NamePart;
			for (const TCHAR Character : Text)
			{
				if (FChar::IsAlnum(Character))
				{
					NamePart.AppendChar(Character);
				}
				else if (!NamePart.IsEmpty() && !NamePart.EndsWith(TEXT("_")))
				{
					NamePart.AppendChar(TEXT('_'));
				}
			}
			NamePart.RemoveFromEnd(TEXT("_"));
			return NamePart;
		}

		/** T_<Material or function>_<Node>[_<Output>], with the asset's "M_" or "MF_" prefix dropped. */
		FString MakeDefaultTextureName(const UObject* EditedAsset, const FText& NodeTitle, const FOutput& Output)
		{
			FString AssetName = EditedAsset->GetName();
			AssetName.RemoveFromStart(TEXT("MF_"));
			AssetName.RemoveFromStart(TEXT("M_"));
			FString TextureName = TEXT("T_") + ToAssetNamePart(AssetName) + TEXT("_") + ToAssetNamePart(NodeTitle.ToString().Left(32));
			if (!Output.Label.EqualTo(NodeTitle))
			{
				TextureName += TEXT("_") + ToAssetNamePart(Output.Label.ToString());
			}
			return TextureName;
		}

		/** Asks where to save the texture. Returns false if cancelled. */
		bool AskTextureObjectPath(const UObject* EditedAsset, const FString& DefaultName, FString& OutObjectPath)
		{
			FSaveAssetDialogConfig DialogConfig;
			DialogConfig.DialogTitleOverride = LOCTEXT("SaveTitle", "Save Node Texture");
			DialogConfig.DefaultPath = FPackageName::GetLongPackagePath(EditedAsset->GetOutermost()->GetName());
			DialogConfig.DefaultAssetName = DefaultName;
			DialogConfig.AssetClassNames.Add(UTexture2D::StaticClass()->GetClassPathName());
			DialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;	// re-exporting updates the texture

			IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
			OutObjectPath = ContentBrowser.CreateModalSaveAssetDialog(DialogConfig);
			return !OutObjectPath.IsEmpty();
		}

		/** The copy of Original among Expressions (duplicated expressions keep their names), or null. */
		template <typename TExpressionRange>
		UMaterialExpression* FindCopiedExpression(const TExpressionRange& Expressions, const UMaterialExpression* Original)
		{
			for (UMaterialExpression* Candidate : Expressions)
			{
				if (Candidate && Candidate->GetFName() == Original->GetFName())
				{
					return Candidate;
				}
			}
			return nullptr;
		}

		/** Marks a bake object transient and roots it: nothing else references it while the progress dialog lets the editor tick. */
		void RootAsTransient(UObject* Object)
		{
			Object->SetFlags(RF_Transient);
			Object->AddToRoot();
		}

		/** A transient Unlit material whose emissive is the exported value, and the code that renders it to pixels. */
		class FNodeBaker
		{
		public:
			/** Node is in a material: bake from a copy of the material being edited. */
			bool InitFromMaterial(UMaterial* EditedMaterial, const UMaterialExpression* Node, int32 OutputIndex, FText& OutError)
			{
				BakeMaterial = DuplicateObject<UMaterial>(EditedMaterial, GetTransientPackage(),
					MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("VrrNodeBake")));
				RootAsTransient(BakeMaterial);
				ValueExpression = FindCopiedExpression(BakeMaterial->GetExpressions(), Node);
				ValueOutputIndex = OutputIndex;
				if (!ValueExpression)
				{
					OutError = LOCTEXT("NodeNotFound", "Couldn't find the node in a copy of the material.");
					return false;
				}
				MakeUnlitSurface();
				return true;
			}

			/**
			 * Node is in a material function: copy the function, give the copy an extra output fed by the node, and call
			 * it from a new material. Like any call, its inputs only have values when "Use Preview Value as Default" is
			 * on (see FindFunctionInputsWithoutDefault).
			 */
			bool InitFromFunction(UMaterialFunction* EditedFunction, const UMaterialExpression* Node, int32 OutputIndex, FText& OutError)
			{
				BakeFunction = DuplicateObject<UMaterialFunction>(EditedFunction, GetTransientPackage(),
					MakeUniqueObjectName(GetTransientPackage(), UMaterialFunction::StaticClass(), TEXT("VrrNodeBakeFunction")));
				RootAsTransient(BakeFunction);
				UMaterialExpression* CopiedNode = FindCopiedExpression(BakeFunction->GetExpressions(), Node);
				if (!CopiedNode)
				{
					OutError = LOCTEXT("FunctionNodeNotFound", "Couldn't find the node in a copy of the function.");
					return false;
				}
				UMaterialExpressionFunctionOutput* BakeOutput = Cast<UMaterialExpressionFunctionOutput>(
					UMaterialEditingLibrary::CreateMaterialExpressionInFunction(BakeFunction, UMaterialExpressionFunctionOutput::StaticClass()));
				BakeOutput->OutputName = TEXT("VrrBakeOutput");
				BakeOutput->SortPriority = TNumericLimits<int32>::Max();
				BakeOutput->A.Connect(OutputIndex, CopiedNode);

				BakeMaterial = NewObject<UMaterial>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("VrrNodeBake")));
				RootAsTransient(BakeMaterial);
				UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(
					UMaterialEditingLibrary::CreateMaterialExpression(BakeMaterial, UMaterialExpressionMaterialFunctionCall::StaticClass()));
				FunctionCall->SetMaterialFunction(BakeFunction);
				for (int32 CallOutputIndex = 0; CallOutputIndex < FunctionCall->FunctionOutputs.Num(); ++CallOutputIndex)
				{
					if (FunctionCall->FunctionOutputs[CallOutputIndex].ExpressionOutput == BakeOutput)
					{
						ValueExpression = FunctionCall;
						ValueOutputIndex = CallOutputIndex;
						break;
					}
				}
				if (!ValueExpression)
				{
					OutError = LOCTEXT("FunctionCallFailed", "Couldn't call a copy of the function.");
					return false;
				}
				MakeUnlitSurface();
				return true;
			}

			~FNodeBaker()
			{
				for (UObject* BakeObject : { static_cast<UObject*>(BakeMaterial), static_cast<UObject*>(BakeFunction) })
				{
					if (BakeObject)
					{
						BakeObject->RemoveFromRoot();
						BakeObject->MarkAsGarbage();
					}
				}
			}

			/** Draws the value, or only its alpha, into OutPixels (Resolution x Resolution, linear floats). */
			bool RenderPixels(bool bAlphaOnly, int32 Resolution, TArray<FLinearColor>& OutPixels, FText& OutError)
			{
				ConnectEmissive(bAlphaOnly);
				UMaterialEditingLibrary::RecompileMaterial(BakeMaterial);

				UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!EditorWorld)
				{
					OutError = LOCTEXT("NoWorld", "There's no editor world to render in.");
					return false;
				}
				if (!ValidateShaders(EditorWorld, OutError))
				{
					return false;
				}
				LoadSampledTextures();

				UTextureRenderTarget2D* RenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(EditorWorld, Resolution, Resolution, RTF_RGBA32f, FLinearColor::Black, false);
				if (!RenderTarget)
				{
					OutError = LOCTEXT("NoTarget", "Couldn't create the render target.");
					return false;
				}
				UKismetRenderingLibrary::DrawMaterialToRenderTarget(EditorWorld, RenderTarget, BakeMaterial);
				FTextureRenderTargetResource* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
				const bool bPixelsRead = RenderTargetResource && RenderTargetResource->ReadLinearColorPixels(OutPixels);	// RCM_MinMax keeps values outside 0-1
				RenderTarget->MarkAsGarbage();
				if (!bPixelsRead || OutPixels.Num() != Resolution * Resolution)
				{
					OutError = LOCTEXT("ReadFailed", "Couldn't read the rendered pixels back.");
					return false;
				}
				return true;
			}

			/** Likely reasons for a black result, found while baking. Shown if the result is black. */
			const TArray<FString>& GetBlackResultHints() const
			{
				return BlackResultHints;
			}

		private:
			/** Turns the bake material into an opaque Unlit surface with nothing wired yet. */
			void MakeUnlitSurface()
			{
				BakeMaterial->MaterialDomain = MD_Surface;
				BakeMaterial->BlendMode = BLEND_Opaque;
				// SetShadingModel ensures the material's previous model is below MSM_NUM. "From Material Expression" isn't
				// (seen without Substrate), so set the private field directly first.
				if (FByteProperty* ShadingModelProperty = FindFProperty<FByteProperty>(UMaterial::StaticClass(), TEXT("ShadingModel")))
				{
					*ShadingModelProperty->ContainerPtrToValuePtr<uint8>(BakeMaterial) = MSM_Unlit;
				}
				BakeMaterial->SetShadingModel(MSM_Unlit);
				BakeMaterial->bUseMaterialAttributes = false;
				for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
				{
					FMaterialInputDescription InputDescription;
					if (BakeMaterial->GetExpressionInputDescription(static_cast<EMaterialProperty>(PropertyIndex), InputDescription) && InputDescription.Input)
					{
						InputDescription.Input->Expression = nullptr;
					}
				}

				if (Substrate::IsSubstrateEnabled())
				{
					UnlitBSDF = Cast<UMaterialExpressionSubstrateUnlitBSDF>(
						UMaterialEditingLibrary::CreateMaterialExpression(BakeMaterial, UMaterialExpressionSubstrateUnlitBSDF::StaticClass()));
					BakeMaterial->GetEditorOnlyData()->FrontMaterial.Connect(0, UnlitBSDF);
				}
				else
				{
					// The material editor's copy sets UseConstant on unconnected outputs, and a constant wins over a
					// connected expression, so the bake would compile the constant (black) instead of the node.
					BakeMaterial->GetEditorOnlyData()->EmissiveColor.UseConstant = false;
				}
			}

			/** Wires the value, or a mask of its alpha, into the Unlit emissive. */
			void ConnectEmissive(bool bAlphaOnly)
			{
				UMaterialExpression* SourceExpression = ValueExpression;
				int32 SourceOutputIndex = ValueOutputIndex;
				if (bAlphaOnly)
				{
					if (!AlphaMask)
					{
						AlphaMask = Cast<UMaterialExpressionComponentMask>(
							UMaterialEditingLibrary::CreateMaterialExpression(BakeMaterial, UMaterialExpressionComponentMask::StaticClass()));
						AlphaMask->R = false;
						AlphaMask->G = false;
						AlphaMask->B = false;
						AlphaMask->A = true;
						AlphaMask->Input.Connect(ValueOutputIndex, ValueExpression);
					}
					SourceExpression = AlphaMask;
					SourceOutputIndex = 0;
				}
				FExpressionInput& Emissive = UnlitBSDF ? UnlitBSDF->EmissiveColor : BakeMaterial->GetEditorOnlyData()->EmissiveColor;
				Emissive.Connect(SourceOutputIndex, SourceExpression);
			}

			/**
			 * The bake material needs shaders on the platform the editor world draws with and on the editor's highest
			 * one. They differ under a Preview Rendering Level, and a material without shaders there is silently drawn
			 * as the default material (black emissive). Waits for this material's shaders only.
			 */
			bool ValidateShaders(UWorld* EditorWorld, FText& OutError)
			{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
				TArray<ERHIFeatureLevel::Type, TInlineAllocator<2>> ShaderTargets;
#if VRR_UE_5_2
				ShaderTargets.Add(EditorWorld->FeatureLevel);
#else
				ShaderTargets.Add(EditorWorld->GetFeatureLevel());
#endif
				ShaderTargets.AddUnique(GMaxRHIFeatureLevel);
				auto GetTargetName = [](ERHIFeatureLevel::Type FeatureLevel)
				{
					FString FeatureLevelName;
					GetFeatureLevelName(FeatureLevel, FeatureLevelName);
					return FeatureLevelName;
				};
#else
				TArray<EShaderPlatform, TInlineAllocator<2>> ShaderTargets;
				ShaderTargets.Add(GetFeatureLevelShaderPlatform(EditorWorld->GetFeatureLevel()));
				ShaderTargets.AddUnique(GMaxRHIShaderPlatform);
				auto GetTargetName = [](EShaderPlatform ShaderPlatform) { return FDataDrivenShaderPlatformInfo::GetName(ShaderPlatform).ToString(); };
#endif
				UE_LOG(LogVrrExportNode, Log, TEXT("Baking %s: the editor world draws with %s, Substrate %s."),
					*BakeMaterial->GetName(), *GetTargetName(ShaderTargets[0]), Substrate::IsSubstrateEnabled() ? TEXT("on") : TEXT("off"));

				for (const auto ShaderTarget : ShaderTargets)
				{
					const FString TargetName = GetTargetName(ShaderTarget);
					FMaterialResource* Resource = BakeMaterial->GetMaterialResource(ShaderTarget);
					if (!Resource)
					{
						OutError = FText::Format(LOCTEXT("NoResource", "The node wasn't compiled for {0}, which the editor draws with."), FText::FromString(TargetName));
						return false;
					}
					Resource->FinishCompilation();
					if (!Resource->GetCompileErrors().IsEmpty())
					{
						const FString CompileErrors = FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
						UE_LOG(LogVrrExportNode, Warning, TEXT("Compile errors on %s:\n%s"), *TargetName, *CompileErrors);
						OutError = FText::Format(LOCTEXT("CompileFailed", "The node doesn't compile on its own as an Unlit surface on {0}:\n{1}"),
							FText::FromString(TargetName), FText::FromString(CompileErrors));
						return false;
					}
					if (!Resource->GetGameThreadShaderMap())
					{
						OutError = FText::Format(LOCTEXT("NoShaderMap", "The node has no shaders for {0}, so it would draw as the default material."), FText::FromString(TargetName));
						return false;
					}
					if (!Resource->IsGameThreadShaderMapComplete())
					{
						BlackResultHints.AddUnique(FString::Printf(TEXT("Shaders for %s are incomplete; the editor may have drawn the default material."), *TargetName));
					}
				}
				return true;
			}

			/** Finishes building the sampled textures and streams in all their mips, so the bake doesn't read placeholders. */
			void LoadSampledTextures()
			{
				TArray<UTexture*> SampledTextures;
				for (UObject* ReferencedObject : BakeMaterial->GetReferencedTextures())
				{
					if (UTexture* Texture = Cast<UTexture>(ReferencedObject))
					{
						SampledTextures.Add(Texture);
					}
				}
				FTextureCompilingManager::Get().FinishCompilation(SampledTextures);
				for (UTexture* Texture : SampledTextures)
				{
					Texture->SetForceMipLevelsToBeResident(30.0f);
					Texture->WaitForStreaming();
					if (Texture->IsCurrentlyVirtualTextured())
					{
						BlackResultHints.AddUnique(FString::Printf(
							TEXT("%s is a virtual texture; its pages may not be loaded outside a viewport. Turn off Virtual Texture Streaming on it to bake."),
							*Texture->GetName()));
					}
				}
			}

			UMaterial* BakeMaterial = nullptr;
			UMaterialFunction* BakeFunction = nullptr;
			UMaterialExpression* ValueExpression = nullptr;
			int32 ValueOutputIndex = 0;
			UMaterialExpressionSubstrateUnlitBSDF* UnlitBSDF = nullptr;
			UMaterialExpressionComponentMask* AlphaMask = nullptr;
			TArray<FString> BlackResultHints;
		};

		/** Renders the value into OutPixels, alpha included for 4 channels, with unused channels filled in for the texture. */
		bool BakePixels(FNodeBaker& Baker, int32 ChannelCount, int32 Resolution, FScopedSlowTask& Progress, TArray<FLinearColor>& OutPixels, FText& OutError)
		{
			Progress.EnterProgressFrame(1.0f, LOCTEXT("BakingColor", "Compiling and rendering the node..."));
			if (!Baker.RenderPixels(false, Resolution, OutPixels, OutError))
			{
				return false;
			}

			if (ChannelCount == 4)
			{
				Progress.EnterProgressFrame(1.0f, LOCTEXT("BakingAlpha", "Rendering alpha..."));
				TArray<FLinearColor> AlphaPixels;
				if (!Baker.RenderPixels(true, Resolution, AlphaPixels, OutError))
				{
					return false;
				}
				for (int32 PixelIndex = 0; PixelIndex < OutPixels.Num(); ++PixelIndex)
				{
					OutPixels[PixelIndex].A = AlphaPixels[PixelIndex].R;
				}
				return true;
			}

			for (FLinearColor& Pixel : OutPixels)
			{
				Pixel.A = 1.0f;
				if (ChannelCount == 1)
				{
					Pixel.G = Pixel.R;	// grayscale
					Pixel.B = Pixel.R;
				}
				else if (ChannelCount == 2)
				{
					Pixel.B = 0.0f;
				}
			}
			return true;
		}

		FValueRange GetValueRange(const TArray<FLinearColor>& Pixels, int32 ChannelCount)
		{
			FValueRange Range;
			Range.Min = TNumericLimits<float>::Max();
			Range.Max = TNumericLimits<float>::Lowest();
			for (const FLinearColor& Pixel : Pixels)
			{
				for (int32 ChannelIndex = 0; ChannelIndex < FMath::Min(ChannelCount, 4); ++ChannelIndex)
				{
					const float Value = Pixel.Component(ChannelIndex);
					Range.Min = FMath::Min(Range.Min, Value);
					Range.Max = FMath::Max(Range.Max, Value);
				}
			}
			return Range;
		}

		/** Asks whether to save a result that is 0 in every channel, listing likely causes. */
		bool ConfirmSaveBlackResult(const FNodeBaker& Baker)
		{
			FString FoundCauses = FString::Join(Baker.GetBlackResultHints(), TEXT("\n- "));
			if (!FoundCauses.IsEmpty())
			{
				FoundCauses = TEXT("\n- ") + FoundCauses;
			}
			UE_LOG(LogVrrExportNode, Warning, TEXT("The bake came out black in every channel.%s"), *FoundCauses);
			const FText Message = FText::Format(LOCTEXT("BlackResult",
				"The node rendered black in every channel. That usually means the bake failed rather than the node being black:\n\n"
				"- It reads something the bake doesn't have (world position, vertex data, scene textures, per-instance data).\n"
				"- A sampled texture isn't loaded (virtual textures especially).{0}\n\n"
				"Save the black texture anyway?"), FText::FromString(FoundCauses));
			return VrrCompat::OpenDialog(EAppMsgType::YesNo, Message, LOCTEXT("BlackResultTitle", "Export Node to Texture")) == EAppReturnType::Yes;
		}

		/** Writes Pixels into the texture at ObjectPath, created or updated in place if it's already a texture. */
		UTexture2D* WriteTexture(const FString& ObjectPath, const FExportOptions& Options, int32 ChannelCount, const TArray<FLinearColor>& Pixels, FText& OutError)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			const FString AssetName = FPackageName::GetShortName(PackageName);
			const int32 Resolution = Options.Resolution;

			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const bool bNewTexture = Texture == nullptr;
			if (bNewTexture)
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
			if (Options.Format == EFormat::HDR)
			{
				TArray<FFloat16Color> SourcePixels;
				SourcePixels.Reserve(Pixels.Num());
				for (const FLinearColor& Pixel : Pixels)
				{
					SourcePixels.Add(FFloat16Color(Pixel));
				}
				Texture->Source.Init(Resolution, Resolution, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(SourcePixels.GetData()));
				Texture->SRGB = false;
				Texture->CompressionSettings = ChannelCount == 1 ? TC_HalfFloat : TC_HDR;
			}
			else
			{
				const bool bSRGB = Options.Format == EFormat::Color;
				TArray<FColor> SourcePixels;
				SourcePixels.Reserve(Pixels.Num());
				for (const FLinearColor& Pixel : Pixels)
				{
					SourcePixels.Add(Pixel.ToFColor(bSRGB));	// clamps to 0-1
				}
				Texture->Source.Init(Resolution, Resolution, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(SourcePixels.GetData()));
				Texture->SRGB = bSRGB;
				if (bSRGB)
				{
					Texture->CompressionSettings = TC_Default;
				}
				else
				{
					Texture->CompressionSettings = ChannelCount == 1 ? TC_Grayscale : TC_Masks;
				}
			}
			Texture->CompressionNoAlpha = ChannelCount < 4;
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
			if (bNewTexture)
			{
				FAssetRegistryModule::AssetCreated(Texture);
			}
			return Texture;
		}

		void NotifyExported(const UTexture2D* Texture, const FText& ExportName, const FExportOptions& Options, const FValueRange& Range)
		{
			FNumberFormattingOptions ThreeDecimals;
			ThreeDecimals.SetMaximumFractionalDigits(3);
			FText Message = FText::Format(LOCTEXT("Exported", "Exported {0} to {1} ({2}). Values range {3} to {4}."),
				ExportName, FText::FromString(Texture->GetName()), FormatResolution(Options.Resolution),
				FText::AsNumber(Range.Min, &ThreeDecimals), FText::AsNumber(Range.Max, &ThreeDecimals));
			const bool bClamped = Options.Format != EFormat::HDR && (Range.Min < -0.001f || Range.Max > 1.001f);
			if (bClamped)
			{
				Message = FText::Format(LOCTEXT("Clamped", "{0} Values outside 0-1 were clamped; export as HDR to keep them."), Message);
			}
			VrrGraph::Notify(Message, true);
		}
	}

	TArray<FOutput> GetExportableOutputs(const UEdGraphNode* Node)
	{
		TArray<FOutput> ExportableOutputs;
		UMaterialExpression* Expression = GetExpression(Node);
		if (!Expression)
		{
			return ExportableOutputs;
		}
		const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
		for (int32 OutputIndex = 0; OutputIndex < Outputs.Num(); ++OutputIndex)
		{
			if ((static_cast<uint64>(VrrCompat::OutputValueType(Expression, OutputIndex)) & NonValueTypes) != 0)
			{
				continue;
			}
			const VrrReroute::EKind Kind = VrrReroute::InferKind(Expression, OutputIndex);
			const bool bNonValueKind = Kind == VrrReroute::EKind::Bool || Kind == VrrReroute::EKind::Texture
				|| Kind == VrrReroute::EKind::MaterialAttributes || Kind == VrrReroute::EKind::ShadingModel;
			if (bNonValueKind)
			{
				continue;
			}
			FText Label = FText::FromName(Outputs[OutputIndex].OutputName);
			if (Outputs[OutputIndex].OutputName.IsNone())
			{
				Label = Outputs.Num() == 1 ? Node->GetNodeTitle(ENodeTitleType::ListView) : FText::Format(LOCTEXT("OutputN", "Output {0}"), OutputIndex);
			}
			ExportableOutputs.Add({ OutputIndex, Label });
		}
		return ExportableOutputs;
	}

	TArray<FName> FindFunctionInputsWithoutDefault(const UEdGraphNode* Node)
	{
		TArray<FName> InputsWithoutDefault;
		TSet<UMaterialExpression*> Visited;
		TArray<UMaterialExpression*> Pending;
		if (UMaterialExpression* StartExpression = GetExpression(Node))
		{
			Pending.Add(StartExpression);
		}
		while (!Pending.IsEmpty())
		{
			UMaterialExpression* Current = Pending.Pop(VRR_NO_SHRINK);
			if (!Current || Visited.Contains(Current))
			{
				continue;
			}
			Visited.Add(Current);
			if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Current))
			{
				if (!FunctionInput->bUsePreviewValueAsDefault)
				{
					InputsWithoutDefault.AddUnique(FunctionInput->InputName);
					continue;	// its preview graph won't be used
				}
			}
			if (const UMaterialExpressionNamedRerouteUsage* RerouteUsage = Cast<UMaterialExpressionNamedRerouteUsage>(Current))
			{
				Pending.Add(RerouteUsage->Declaration);
			}
			for (FExpressionInputIterator InputIt{ Current }; InputIt; ++InputIt)
			{
				Pending.Add(InputIt.Input->Expression);
			}
		}
		return InputsWithoutDefault;
	}

	int32 FindOutputIndexOfPin(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
		if (!MaterialNode || !Pin || Pin->Direction != EGPD_Output || !MaterialNode->MaterialExpression)
		{
			return INDEX_NONE;
		}
		for (int32 OutputIndex = 0; OutputIndex < MaterialNode->MaterialExpression->GetOutputs().Num(); ++OutputIndex)
		{
			if (MaterialNode->GetOutputPin(OutputIndex) == Pin)
			{
				return OutputIndex;
			}
		}
		return INDEX_NONE;
	}

	void Export(UMaterialGraph* Graph, const UEdGraphNode* Node, int32 OutputIndex)
	{
		UMaterialExpression* Expression = GetExpression(Node);
		TSharedPtr<IMaterialEditor> MaterialEditor = VrrGraph::GetEditor(Graph);
		UMaterialFunction* EditedFunction = Graph ? Graph->MaterialFunction.Get() : nullptr;
		if (!Graph || (!Graph->Material && !EditedFunction) || !Expression || !MaterialEditor)
		{
			return;
		}
		if (EditedFunction)
		{
			const TArray<FName> InputsWithoutDefault = FindFunctionInputsWithoutDefault(Node);
			if (!InputsWithoutDefault.IsEmpty())
			{
				TArray<FString> InputNames;
				Algo::Transform(InputsWithoutDefault, InputNames, [](FName InputName) { return InputName.ToString(); });
				VrrGraph::Notify(FText::Format(LOCTEXT("NoDefault", "Export Node to Texture: turn on \"Use Preview Value as Default\" on function input(s) {0}, which this node reads."),
					FText::FromString(FString::Join(InputNames, TEXT(", ")))), false);
				return;
			}
		}
		const TArray<FOutput> ExportableOutputs = GetExportableOutputs(Node);
		const FOutput* Output = ExportableOutputs.FindByPredicate([OutputIndex](const FOutput& Candidate) { return Candidate.Index == OutputIndex; });
		if (!Output)
		{
			return;
		}
		const int32 ChannelCount = GetChannelCount(Expression, OutputIndex);
		const FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
		const FText ExportName = Output->Label.EqualTo(NodeTitle) ? NodeTitle : FText::Format(LOCTEXT("NodeAndOutput", "{0}: {1}"), NodeTitle, Output->Label);

		// 1. Options
		FExportOptions Options = LoadOptions(ChannelCount);
		if (!AskExportOptions(ExportName, ChannelCount, Options))
		{
			return;
		}
		SaveOptions(Options, ChannelCount);

		// 2. Destination, named after the asset (not the editor's transient copy)
		const TArray<UObject*>* EditedObjects = MaterialEditor->GetObjectsCurrentlyBeingEdited();
		const UObject* EditedAsset = EditedObjects && EditedObjects->Num() > 0 ? (*EditedObjects)[0] : VrrCompat::MaterialOrFunctionOf(Graph);
		FString TextureObjectPath;
		if (!AskTextureObjectPath(EditedAsset, MakeDefaultTextureName(EditedAsset, NodeTitle, *Output), TextureObjectPath))
		{
			return;
		}

		// 3. Bake
		const float ProgressSteps = ChannelCount == 4 ? 3.0f : 2.0f;
		FScopedSlowTask Progress(ProgressSteps, LOCTEXT("Baking", "Exporting node to texture..."));
		Progress.MakeDialog();
		FNodeBaker Baker;
		FText Error;
		TArray<FLinearColor> Pixels;
		const bool bBakerReady = EditedFunction
			? Baker.InitFromFunction(EditedFunction, Expression, OutputIndex, Error)
			: Baker.InitFromMaterial(Graph->Material, Expression, OutputIndex, Error);
		if (!bBakerReady || !BakePixels(Baker, ChannelCount, Options.Resolution, Progress, Pixels, Error))
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("ExportFailed", "Export Node to Texture failed: {0}"), Error), false);
			return;
		}
		const FValueRange Range = GetValueRange(Pixels, ChannelCount);
		const bool bAllBlack = Range.Min == 0.0f && Range.Max == 0.0f;
		if (bAllBlack && !ConfirmSaveBlackResult(Baker))
		{
			VrrGraph::Notify(LOCTEXT("BlackCancelled", "Export Node to Texture: the node rendered black, so nothing was saved. See the Output Log (LogVrrExportNode)."), false);
			return;
		}

		// 4. Write
		Progress.EnterProgressFrame(1.0f, LOCTEXT("Writing", "Writing the texture..."));
		UTexture2D* Texture = WriteTexture(TextureObjectPath, Options, ChannelCount, Pixels, Error);
		if (!Texture)
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("ExportFailed", "Export Node to Texture failed: {0}"), Error), false);
			return;
		}
		IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
		ContentBrowser.SyncBrowserToAssets(TArray<UObject*>{ Texture });
		NotifyExported(Texture, ExportName, Options, Range);
	}
}

#undef LOCTEXT_NAMESPACE
