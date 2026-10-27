/*
 * VRR_MaterialsEditor: editor-only tools for VRR_Materials.
 *
 * Adds "Create Parameter Data Function" to the material graph's right-click menu (see Vrr_ParameterDataFunction).
 * The entry goes into the material node context menu through ToolMenus, so no engine code is touched; it only
 * shows when a node was right-clicked in a material (not a material function) and the selection has parameters.
 */

#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "EdGraph/EdGraphNode.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Vrr_ParameterDataFunction.h"

#define LOCTEXT_NAMESPACE "VRR_MaterialsEditor"

class FVRR_MaterialsEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FVRR_MaterialsEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		// Right-click menu of material expression nodes. Node menus are named after the node class
		// ("GraphEditor.GraphNodeContextMenu.<Class>") and chain up to the generic EdGraphSchema menu, skipping the
		// material schema's own menu (that one only opens on empty graph space), so the node menu is the one to extend.
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("GraphEditor.GraphNodeContextMenu.MaterialGraphNode");
		Menu->AddDynamicSection("VrrParameterData", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
		{
			// 1. Only when right-clicking a node in a material graph (material functions are left out)
			const UGraphNodeContextMenuContext* Context = InMenu->FindContext<UGraphNodeContextMenuContext>();
			UMaterialGraph* Graph = Context ? const_cast<UMaterialGraph*>(Cast<UMaterialGraph>(Context->Graph)) : nullptr;
			if (!Context || !Context->Node || !Graph || !Graph->Material || Graph->MaterialFunction)
			{
				return;
			}

			// 2. Only when the selection has parameters this tool can move
			const int32 NumParameters = VrrParameterData::GetSelectedParameters(Graph).Num();
			if (NumParameters == 0)
			{
				return;
			}

			TWeakObjectPtr<UMaterialGraph> WeakGraph(Graph);
			FToolMenuSection& Section = InMenu->AddSection("VrrParameterData", LOCTEXT("SectionLabel", "VRR"));
			Section.AddMenuEntry(
				"CreateParameterDataFunction",
				LOCTEXT("CreateLabel", "Create Parameter Data Function"),
				FText::Format(LOCTEXT("CreateTooltip",
					"Move the {0} selected parameter(s) (scalar, vector, texture sample, texture object with its samplers, and "
					"appends of them) into a new material function, and replace them here with the function and named reroutes "
					"(s_, vec2_, vec3_, vec4_)."), NumParameters),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WeakGraph]()
				{
					if (UMaterialGraph* G = WeakGraph.Get())
					{
						VrrParameterData::CreateFromSelection(G);
					}
				})));
		}));
	}
};

IMPLEMENT_MODULE(FVRR_MaterialsEditorModule, VRR_MaterialsEditor)

#undef LOCTEXT_NAMESPACE
