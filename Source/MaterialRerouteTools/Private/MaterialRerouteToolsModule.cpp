/*
 * MaterialRerouteTools: editor-only material graph tools, in a "VRR" section of the material graph's right-click
 * menus (materials only, not material functions):
 *
 *   Expression nodes, when the selection has parameters:
 *     - Organize Parameters into Named Reroutes (Vrr_OrganizeParameters)
 *     - Create Parameter Data Function, Add to Parameter Data Function > (Vrr_ParameterDataFunction)
 *   Result node and Make / Set Material Attributes nodes:
 *     - Organize Inputs into Named Reroutes (Vrr_OrganizeInputs)
 *
 * The drag-off-a-pin menu gets "Create Prefixed Named Reroute" (node outputs, and the inputs of the nodes above)
 * through the graph schema instead (Vrr_PrefixedReroute, nothing to register here; it also works in functions).
 *
 * Node menus are named after the node class ("GraphEditor.GraphNodeContextMenu.<Class>") and chain up to the generic
 * EdGraphSchema menu, skipping the material schema's own menu (that one only opens on empty graph space), so the node
 * menus are the ones to extend. The Result node has its own class, so its own menu. No engine code is touched.
 */

#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "EdGraph/EdGraphNode.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Vrr_OrganizeInputs.h"
#include "Vrr_OrganizeParameters.h"
#include "Vrr_ParameterDataFunction.h"

#define LOCTEXT_NAMESPACE "MaterialRerouteTools"

namespace
{
	const FName SectionName("Vrr");

	/** The material graph a node was right-clicked in (null for material functions or anything else). */
	UMaterialGraph* MaterialGraphOf(const UGraphNodeContextMenuContext* Context)
	{
		UMaterialGraph* Graph = Context && Context->Node ? const_cast<UMaterialGraph*>(Cast<UMaterialGraph>(Context->Graph)) : nullptr;
		return Graph && Graph->Material && !Graph->MaterialFunction ? Graph : nullptr;
	}

	FToolMenuSection& VrrSection(UToolMenu* Menu)
	{
		return Menu->AddSection(SectionName, LOCTEXT("SectionLabel", "VRR"));	// finds it if already added
	}

	/** Runs Action on the graph if it's still around when the entry is clicked. */
	FExecuteAction OnGraph(UMaterialGraph* Graph, TFunction<void(UMaterialGraph*)> Action)
	{
		return FExecuteAction::CreateLambda([WeakGraph = TWeakObjectPtr<UMaterialGraph>(Graph), Action = MoveTemp(Action)]()
		{
			if (UMaterialGraph* G = WeakGraph.Get())
			{
				Action(G);
			}
		});
	}

	void AddOrganizeInputs(UToolMenu* Menu)
	{
		const UGraphNodeContextMenuContext* Context = Menu->FindContext<UGraphNodeContextMenuContext>();
		UMaterialGraph* Graph = MaterialGraphOf(Context);
		if (!Graph || !VrrOrganizeInputs::CanOrganize(Graph, Context->Node))
		{
			return;
		}
		const int32 NumInputs = VrrOrganizeInputs::CountInputsToOrganize(Graph, Context->Node);
		TWeakObjectPtr<const UEdGraphNode> WeakNode(Context->Node);
		VrrSection(Menu).AddMenuEntry(
			"OrganizeInputsIntoNamedReroutes",
			LOCTEXT("OrganizeInputsLabel", "Organize Inputs into Named Reroutes"),
			FText::Format(LOCTEXT("OrganizeInputsTooltip",
				"Break the {0} wired input(s) of this node into named reroutes named after each input: a declaration beside "
				"the node that feeds it and a usage beside the pin. Inputs that already read a named reroute are left alone."), NumInputs),
			FSlateIcon(),
			FUIAction(
				OnGraph(Graph, [WeakNode](UMaterialGraph* G) { VrrOrganizeInputs::OrganizeInputs(G, WeakNode.Get()); }),
				FCanExecuteAction::CreateLambda([NumInputs]() { return NumInputs > 0; })));
	}

	void AddParameterTools(UToolMenu* Menu)
	{
		UMaterialGraph* Graph = MaterialGraphOf(Menu->FindContext<UGraphNodeContextMenuContext>());
		if (!Graph)
		{
			return;
		}
		const int32 NumToOrganize = VrrOrganizeParameters::GetSelectedParameters(Graph).Num();
		const int32 NumToMove = VrrParameterData::GetSelectedParameters(Graph).Num();

		if (NumToOrganize > 0)
		{
			VrrSection(Menu).AddMenuEntry(
				"OrganizeParametersIntoNamedReroutes",
				LOCTEXT("OrganizeLabel", "Organize Parameters into Named Reroutes"),
				FText::Format(LOCTEXT("OrganizeTooltip",
					"Gather the {0} selected parameter(s) into a column at the top left of the graph, give each value read from "
					"them a named reroute (b_, s_, vec2_, vec3_, vec4_, tex_) and read it through reroute usages. Other selected "
					"nodes are ignored; same-named parameters are merged."), NumToOrganize),
				FSlateIcon(),
				FUIAction(OnGraph(Graph, &VrrOrganizeParameters::OrganizeSelection)));
		}
		if (NumToMove == 0)
		{
			return;
		}
		FToolMenuSection& Section = VrrSection(Menu);
		Section.AddMenuEntry(
			"CreateParameterDataFunction",
			LOCTEXT("CreateLabel", "Create Parameter Data Function"),
			FText::Format(LOCTEXT("CreateTooltip",
				"Move the {0} selected parameter(s) (scalar, vector, texture sample, texture object with its samplers, and "
				"appends of them) into a new material function, and replace them here with the function and named reroutes "
				"(s_, vec2_, vec3_, vec4_)."), NumToMove),
			FSlateIcon(),
			FUIAction(OnGraph(Graph, &VrrParameterData::CreateFromSelection)));

		// One entry per parameter data function the material already calls
		TArray<TWeakObjectPtr<UMaterialExpressionMaterialFunctionCall>> Calls;
		for (UMaterialExpressionMaterialFunctionCall* Call : VrrParameterData::GetParameterDataCalls(Graph))
		{
			Calls.Add(Call);
		}
		if (Calls.IsEmpty())
		{
			return;
		}
		Section.AddSubMenu(
			"AddToParameterDataFunction",
			LOCTEXT("AddLabel", "Add to Parameter Data Function"),
			FText::Format(LOCTEXT("AddTooltip",
				"Move the {0} selected parameter(s) into a parameter data function this material already calls. New outputs "
				"are added after the existing ones and read through new named reroutes; parameters the function already has "
				"are reused."), NumToMove),
			FNewToolMenuDelegate::CreateLambda([WeakGraph = TWeakObjectPtr<UMaterialGraph>(Graph), Calls](UToolMenu* SubMenu)
			{
				UMaterialGraph* G = WeakGraph.Get();
				FToolMenuSection& Functions = SubMenu->AddSection("Functions", LOCTEXT("FunctionsLabel", "Functions"));
				for (const TWeakObjectPtr<UMaterialExpressionMaterialFunctionCall>& WeakCall : Calls)
				{
					const UMaterialExpressionMaterialFunctionCall* Call = WeakCall.Get();
					if (!G || !Call || !Call->MaterialFunction)
					{
						continue;
					}
					Functions.AddMenuEntry(
						FName(Call->MaterialFunction->GetName()),
						FText::FromString(Call->MaterialFunction->GetName()),
						FText::FromString(Call->MaterialFunction->GetPathName()),
						FSlateIcon(),
						FUIAction(OnGraph(G, [WeakCall](UMaterialGraph* Target) { VrrParameterData::AddToFunction(Target, WeakCall.Get()); })));
				}
			}));
	}
}

class FMaterialRerouteToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMaterialRerouteToolsModule::RegisterMenus));
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
		UToolMenu* NodeMenu = UToolMenus::Get()->ExtendMenu("GraphEditor.GraphNodeContextMenu.MaterialGraphNode");
		UToolMenu* ResultMenu = UToolMenus::Get()->ExtendMenu("GraphEditor.GraphNodeContextMenu.MaterialGraphNode_Root");

		NodeMenu->AddDynamicSection("VrrOrganizeInputs", FNewToolMenuDelegate::CreateStatic(&AddOrganizeInputs));
		ResultMenu->AddDynamicSection("VrrOrganizeInputs", FNewToolMenuDelegate::CreateStatic(&AddOrganizeInputs));
		NodeMenu->AddDynamicSection("VrrParameterTools", FNewToolMenuDelegate::CreateStatic(&AddParameterTools));
	}
};

IMPLEMENT_MODULE(FMaterialRerouteToolsModule, MaterialRerouteTools)

#undef LOCTEXT_NAMESPACE
