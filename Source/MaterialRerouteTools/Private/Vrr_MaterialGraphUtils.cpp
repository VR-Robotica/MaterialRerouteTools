// Copyright VR Robotica. All Rights Reserved.

#include "Vrr_MaterialGraphUtils.h"

#include "Framework/Notifications/NotificationManager.h"
#include "IMaterialEditor.h"
#include "MaterialEditingLibrary.h"
#include "MaterialEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace VrrGraph
{
	FReaders MapReaders(UMaterial* Material)
	{
		FReaders Readers;
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

		// Hidden outputs count too: legacy pins while Substrate is on can still hold a link
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
		return Readers;
	}

	TSharedPtr<IMaterialEditor> GetEditor(const UMaterialGraph* Graph)
	{
		return FMaterialEditorUtilities::GetIMaterialEditorForObject(Graph);
	}

	TArray<UMaterialExpression*> GetSelectedExpressions(const UMaterialGraph* Graph, TFunctionRef<bool(UMaterialExpression*)> Filter)
	{
		TArray<UMaterialExpression*> Result;
		if (const TSharedPtr<IMaterialEditor> Editor = GetEditor(Graph))
		{
			for (UObject* Selected : Editor->GetSelectedNodes())
			{
				const UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Selected);
				UMaterialExpression* Expression = Node ? Node->MaterialExpression.Get() : nullptr;
				if (Expression && Filter(Expression))
				{
					Result.Add(Expression);
				}
			}
		}
		return Result;
	}

	bool ByPosition(const UMaterialExpression& A, const UMaterialExpression& B)
	{
		return A.MaterialExpressionEditorY != B.MaterialExpressionEditorY
			? A.MaterialExpressionEditorY < B.MaterialExpressionEditorY
			: A.MaterialExpressionEditorX < B.MaterialExpressionEditorX;
	}

	FParameterChannelNames ChannelNamesOf(const UMaterialExpression* E)
	{
		if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(E))
		{
			return Vector->ChannelNames;
		}
		if (const UMaterialExpressionDynamicParameter* Dynamic = Cast<UMaterialExpressionDynamicParameter>(E))
		{
			// Its Param Names, the defaults ("Param1"...) not counting as names
			FParameterChannelNames Names;
			FText* Channels[] = { &Names.R, &Names.G, &Names.B, &Names.A };
			for (int32 Channel = 0; Channel < 4 && Channel < Dynamic->ParamNames.Num(); ++Channel)
			{
				const FString Name = Dynamic->ParamNames[Channel].TrimStartAndEnd();
				if (Name != FString::Printf(TEXT("Param%d"), Channel + 1))
				{
					*Channels[Channel] = FText::FromString(Name);
				}
			}
			return Names;
		}
		const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(E);
		if (!Sample)
		{
			return FParameterChannelNames();
		}
		const FParameterChannelNames& Own = Sample->ChannelNames;
		const bool bHasOwn = !Own.R.IsEmpty() || !Own.G.IsEmpty() || !Own.B.IsEmpty() || !Own.A.IsEmpty();
		if (!bHasOwn && E->GetClass() == UMaterialExpressionTextureSample::StaticClass())
		{
			if (const UMaterialExpressionTextureSample* Object = Cast<UMaterialExpressionTextureSample>(Sample->TextureObject.Expression))
			{
				return Object->ChannelNames;
			}
		}
		return Own;
	}

	FString ChannelName(const UMaterialExpression* E, const FString& Base, int32 Channel)
	{
		static const TCHAR* Letters[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
		check(Channel >= 0 && Channel < 4);
		const FParameterChannelNames Custom = ChannelNamesOf(E);
		const FText* CustomNames[] = { &Custom.R, &Custom.G, &Custom.B, &Custom.A };
		const FString CustomName = CustomNames[Channel]->ToString().TrimStartAndEnd();
		return CustomName.IsEmpty() ? FString::Printf(TEXT("%s %s"), *Base, Letters[Channel]) : CustomName;
	}

	UMaterialExpressionNamedRerouteDeclaration* CreateDeclaration(UMaterial* Material, UMaterialExpression* Source, int32 Pin,
		int32 X, int32 Y, const FString& Name, const FLinearColor& Color)
	{
		UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Material, nullptr, UMaterialExpressionNamedRerouteDeclaration::StaticClass(), nullptr, X, Y, false));
		if (Declaration)
		{
			Declaration->Input.Connect(Pin, Source);
			Declaration->NodeColor = Color;
			Declaration->SetEditableName(Name);
		}
		return Declaration;
	}

	FIntPoint UsagePositionFor(const UMaterial* Material, const UMaterialExpression* Consumer, int32 Slot)
	{
		const int32 X = Consumer ? Consumer->MaterialExpressionEditorX : Material->EditorX;
		const int32 Y = Consumer ? Consumer->MaterialExpressionEditorY : Material->EditorY;
		return FIntPoint(X - Layout::UsageOffset, Y + Slot * Layout::UsageStep);
	}

	FIntPoint UsagePositionForPin(int32 NodeX, int32 NodeY, int32 Row)
	{
		return FIntPoint(NodeX - Layout::UsageOffset, NodeY + Layout::PinTop + Row * Layout::PinStep);
	}

	UMaterialExpressionNamedRerouteUsage* CreateUsage(UMaterial* Material, UMaterialExpressionNamedRerouteDeclaration* Declaration, FIntPoint Position)
	{
		UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Material, nullptr, UMaterialExpressionNamedRerouteUsage::StaticClass(), nullptr, Position.X, Position.Y, false));
		BindUsage(Usage, Declaration);
		return Usage;
	}

	void BindUsage(UMaterialExpressionNamedRerouteUsage* Usage, UMaterialExpressionNamedRerouteDeclaration* Declaration)
	{
		if (Usage && Declaration)
		{
			Usage->Declaration = Declaration;
			Usage->DeclarationGuid = Declaration->VariableGuid;
		}
	}

	void Rewire(const FLink& Read, UMaterialExpression* Usage)
	{
		if (Read.Consumer)
		{
			Read.Consumer->Modify();
		}
		Read.Input->Expression = Usage;
		Read.Input->OutputIndex = 0;
	}

	void FinishEdit(UMaterialGraph* Graph, IMaterialEditor& Editor, TConstArrayView<const UMaterialExpression*> Select)
	{
		Graph->RebuildGraph();
		Editor.UpdateMaterialAfterGraphChange();
		TSet<const UEdGraphNode*> Nodes;
		for (const UMaterialExpression* E : Select)
		{
			if (E && E->GraphNode)
			{
				Nodes.Add(E->GraphNode);
			}
		}
		Graph->SelectNodeSet(Nodes);
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> ExpressionsOf(const UMaterialGraph* Graph)
	{
		if (Graph && Graph->MaterialFunction)
		{
			return Graph->MaterialFunction->GetExpressions();
		}
		return Graph && Graph->Material ? Graph->Material->GetExpressions() : TConstArrayView<TObjectPtr<UMaterialExpression>>();
	}

	TArray<UMaterialExpressionNamedRerouteDeclaration*> NamedRerouteDeclarations(const UMaterialGraph* Graph)
	{
		TArray<UMaterialExpressionNamedRerouteDeclaration*> Result;
		for (UMaterialExpression* E : ExpressionsOf(Graph))
		{
			if (UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(E))
			{
				Result.Add(Declaration);
			}
		}
		return Result;
	}

	void RedrawAfterRerouteEdit(UMaterialGraph* Graph, TConstArrayView<UMaterialExpressionNamedRerouteDeclaration*> Edited)
	{
		TSet<UEdGraph*> Graphs = { Graph };
		for (const UMaterialExpressionNamedRerouteDeclaration* Declaration : Edited)
		{
			if (Declaration->GraphNode)
			{
				Graphs.Add(Declaration->GraphNode->GetGraph());	// may be a composite's subgraph
			}
		}
		for (UEdGraph* G : Graphs)
		{
			G->NotifyGraphChanged();
		}
		FMaterialEditorUtilities::MarkMaterialDirty(Graph);
	}

	bool IsNamedRerouteNode(const UEdGraphNode* Node)
	{
		const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node);
		return MaterialNode && Cast<UMaterialExpressionNamedRerouteBase>(MaterialNode->MaterialExpression.Get());
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
}
