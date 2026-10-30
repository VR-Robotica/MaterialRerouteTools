// Copyright VR Robotica. All Rights Reserved.

/*
 * MaterialRerouteTools: editor-only material graph tools, in a "Material Reroute Tools" section of the material graph's right-click
 * menus (materials only, not material functions):
 *
 *   Expression nodes, when the selection has parameters:
 *     - Organize Parameters into Named Reroutes (Vrr_OrganizeParameters)
 *     - Create Parameter Data Function, Add to Parameter Data Function > (Vrr_ParameterDataFunction)
 *   Result node and Make / Set Material Attributes nodes:
 *     - Organize Inputs into Named Reroutes (Vrr_OrganizeInputs)
 *   Expression nodes with a value output (and their output pins), in materials and material functions:
 *     - Export Node to Texture... (Vrr_ExportNodeTexture)
 *
 * The drag-off-a-pin menu gets "Create Prefixed Named Reroute" (node outputs, and the inputs of the nodes above)
 * through the graph schema instead (Vrr_PrefixedReroute, nothing to register here; it also works in functions).
 *
 * Node menus are named after the node class ("GraphEditor.GraphNodeContextMenu.<Class>") and chain up to the generic
 * EdGraphSchema menu, skipping the material schema's own menu (that one only opens on empty graph space), so the node
 * menus are the ones to extend. The Result node has its own class, so its own menu. No engine code is touched.
 */

#include "Algo/Transform.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "EdGraph/EdGraphNode.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Vrr_ExportNodeTexture.h"
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
		return Menu->AddSection(SectionName, LOCTEXT("SectionLabel", "Material Reroute Tools"));	// finds it if already added
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

	void AddExportNode(UToolMenu* Menu)
	{
		// Unlike the other node tools, this one also works in material functions
		const UGraphNodeContextMenuContext* Context = Menu->FindContext<UGraphNodeContextMenuContext>();
		UMaterialGraph* Graph = Context && Context->Node ? const_cast<UMaterialGraph*>(Cast<UMaterialGraph>(Context->Graph)) : nullptr;
		if (!Graph || (!Graph->Material && !Graph->MaterialFunction))
		{
			return;
		}
		const TArray<VrrExportNode::FOutput> Outputs = VrrExportNode::GetExportableOutputs(Context->Node);
		if (Outputs.IsEmpty())
		{
			return;
		}

		// In a function, every function input the node reads needs a value of its own
		TArray<FString> Missing;
		if (Graph->MaterialFunction)
		{
			Algo::Transform(VrrExportNode::FunctionInputsWithoutDefault(Context->Node), Missing, [](FName N) { return N.ToString(); });
		}
		const bool bCanExport = Missing.IsEmpty();

		TWeakObjectPtr<const UEdGraphNode> WeakNode(Context->Node);
		auto ExportAction = [Graph, WeakNode, bCanExport](int32 Output)
		{
			return FUIAction(
				OnGraph(Graph, [WeakNode, Output](UMaterialGraph* G) { VrrExportNode::Export(G, WeakNode.Get(), Output); }),
				FCanExecuteAction::CreateLambda([bCanExport]() { return bCanExport; }));
		};
		const FText Tooltip = bCanExport
			? LOCTEXT("ExportTooltip",
				"Render this node's value across a texture's 0-1 UVs and save it as a texture asset (Color, Linear data or HDR). "
				"Picking an existing texture updates it, so the node can be re-exported after changes.")
			: FText::Format(LOCTEXT("ExportNoDefaultTooltip",
				"This node reads function input(s) {0}, which have no value outside a call. Turn on \"Use Preview Value as "
				"Default\" on them to export it."), FText::FromString(FString::Join(Missing, TEXT(", "))));

		// Right-clicked on an output pin: export that one
		const int32 PinOutput = VrrExportNode::OutputIndexOfPin(Context->Node, Context->Pin);
		const VrrExportNode::FOutput* OnPin = Outputs.FindByPredicate([PinOutput](const VrrExportNode::FOutput& O) { return O.Index == PinOutput; });
		if (OnPin || Outputs.Num() == 1)
		{
			const VrrExportNode::FOutput& Only = OnPin ? *OnPin : Outputs[0];
			VrrSection(Menu).AddMenuEntry(
				"ExportNodeToTexture",
				OnPin && Outputs.Num() > 1
					? FText::Format(LOCTEXT("ExportPinLabel", "Export {0} to Texture..."), Only.Label)
					: LOCTEXT("ExportNodeLabel", "Export Node to Texture..."),
				Tooltip, FSlateIcon(), ExportAction(Only.Index));
			return;
		}
		VrrSection(Menu).AddSubMenu(
			"ExportNodeToTexture",
			LOCTEXT("ExportNodeSubLabel", "Export Node to Texture"),
			Tooltip,
			FNewToolMenuDelegate::CreateLambda([Outputs, ExportAction, Tooltip](UToolMenu* SubMenu)
			{
				FToolMenuSection& Section = SubMenu->AddSection("Outputs", LOCTEXT("OutputsLabel", "Outputs"));
				for (const VrrExportNode::FOutput& O : Outputs)
				{
					Section.AddMenuEntry(FName(*FString::Printf(TEXT("Output%d"), O.Index)), FText::Format(LOCTEXT("ExportOutputLabel", "{0}..."), O.Label),
						Tooltip, FSlateIcon(), ExportAction(O.Index));
				}
			}));
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
					"them a named reroute (prefixed and colored by type, see Project Settings > Plugins > Material Reroute Tools) and read it through reroute usages. Other selected "
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
		NodeMenu->AddDynamicSection("VrrExportNode", FNewToolMenuDelegate::CreateStatic(&AddExportNode));
	}
};

IMPLEMENT_MODULE(FMaterialRerouteToolsModule, MaterialRerouteTools)

#undef LOCTEXT_NAMESPACE
