// Copyright VR Robotica. All Rights Reserved.

#include "Vrr_UpdateReroutes.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "EdGraph/EdGraphSchema.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(Vrr_UpdateReroutes)

#define LOCTEXT_NAMESPACE "VrrUpdateReroutes"

namespace
{
	using VrrReroute::EKind;

	// ---- Colors ----

	struct FRecolor
	{
		UMaterialExpressionNamedRerouteDeclaration* Declaration = nullptr;
		FLinearColor Color;
	};

	struct FColorPlan
	{
		TArray<FRecolor> Changes;
		int32 Declarations = 0;
		int32 Untyped = 0;	// no type prefix, no typed input
	};

	FColorPlan PlanColors(const UMaterialGraph* Graph)
	{
		FColorPlan Plan;
		for (UMaterialExpressionNamedRerouteDeclaration* Declaration : VrrGraph::NamedRerouteDeclarations(Graph))
		{
			++Plan.Declarations;
			EKind Kind = VrrReroute::MatchPrefix(Declaration->Name.ToString()).Kind;
			if (Kind == EKind::Unknown)
			{
				Kind = VrrReroute::InferLinkKind(Declaration->Input);
			}
			if (Kind == EKind::Unknown)
			{
				++Plan.Untyped;
				continue;
			}
			const FLinearColor Color = VrrReroute::ColorForLink(Declaration->Input, Kind);
			if (!Declaration->NodeColor.Equals(Color))
			{
				Plan.Changes.Add({ Declaration, Color });
			}
		}
		return Plan;
	}

	// ---- Prefixes ----

	enum class EReason : uint8 { NoPrefix, OldPrefix, WrongType };

	struct FRename
	{
		UMaterialExpressionNamedRerouteDeclaration* Declaration = nullptr;
		FString NewName;
		EKind Kind = EKind::Unknown;	// what it carries
		EKind Named = EKind::Unknown;	// what its old prefix said
		EReason Reason = EReason::NoPrefix;
	};

	struct FPrefixPlan
	{
		TArray<FRename> Renames;
		int32 Declarations = 0;
		int32 Untyped = 0;	// no typed input, no type prefix
	};

	FText KindName(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Bool:               return LOCTEXT("Bool", "Boolean");
		case EKind::Scalar:             return LOCTEXT("Scalar", "Scalar");
		case EKind::Vector2:            return LOCTEXT("Vector2", "Vector 2");
		case EKind::Vector3:            return LOCTEXT("Vector3", "Vector 3");
		case EKind::Vector4:            return LOCTEXT("Vector4", "Vector 4");
		case EKind::Texture:            return LOCTEXT("Texture", "Texture Object");
		case EKind::MaterialAttributes: return LOCTEXT("MaterialAttributes", "Material Attributes");
		case EKind::ShadingModel:       return LOCTEXT("ShadingModel", "Shading Model");
		default:                        return LOCTEXT("Unknown", "Unknown");
		}
	}

	FPrefixPlan PlanPrefixes(const UMaterialGraph* Graph)
	{
		FPrefixPlan Plan;
		for (UMaterialExpressionNamedRerouteDeclaration* Declaration : VrrGraph::NamedRerouteDeclarations(Graph))
		{
			++Plan.Declarations;
			const FString Name = Declaration->Name.ToString();
			const VrrReroute::FPrefixMatch Match = VrrReroute::MatchPrefix(Name);

			// What it carries; an unwired reroute goes by its prefix
			const EKind Wire = VrrReroute::InferLinkKind(Declaration->Input);
			const EKind Kind = Wire != EKind::Unknown ? Wire : Match.Kind;
			if (Kind == EKind::Unknown)
			{
				++Plan.Untyped;
				continue;
			}

			const FString Body = Name.Len() > Match.Length ? Name.RightChop(Match.Length) : FString(TEXT("reroute"));
			FString NewName = VrrReroute::Prefix(Kind) + Body;
			if (NewName != Name)
			{
				const EReason Reason = Match.Length == 0 ? EReason::NoPrefix
					: Match.Kind != EKind::Unknown && Match.Kind != Kind ? EReason::WrongType
					: EReason::OldPrefix;
				Plan.Renames.Add({ Declaration, MoveTemp(NewName), Kind, Match.Kind, Reason });
			}
		}
		Plan.Renames.Sort([](const FRename& A, const FRename& B) { return VrrGraph::ByPosition(*A.Declaration, *B.Declaration); });
		return Plan;
	}

	FText Describe(const FRename& Rename)
	{
		const FText From = FText::FromName(Rename.Declaration->Name);
		const FText To = FText::FromString(Rename.NewName);
		switch (Rename.Reason)
		{
		case EReason::WrongType:
			return FText::Format(LOCTEXT("WrongType", "{0} -> {1}   (named {2}, carries {3})"), From, To, KindName(Rename.Named), KindName(Rename.Kind));
		case EReason::OldPrefix:
			return FText::Format(LOCTEXT("OldPrefix", "{0} -> {1}   (old prefix)"), From, To);
		default:
			return FText::Format(LOCTEXT("NoPrefix", "{0} -> {1}   (no prefix)"), From, To);
		}
	}

	bool ConfirmRenames(const FPrefixPlan& Plan)
	{
		constexpr int32 MaxListed = 30;
		TArray<FText> Lines;
		for (int32 i = 0; i < FMath::Min(Plan.Renames.Num(), MaxListed); ++i)
		{
			Lines.Add(Describe(Plan.Renames[i]));
		}
		if (Plan.Renames.Num() > MaxListed)
		{
			Lines.Add(FText::Format(LOCTEXT("More", "... and {0} more"), Plan.Renames.Num() - MaxListed));
		}
		const FText Message = FText::Format(LOCTEXT("Confirm",
			"Rename {0} named reroute(s) to match the type they carry? Usages follow, and renamed reroutes get their type's color.\n\n{1}"),
			Plan.Renames.Num(), FText::Join(FText::FromString(TEXT("\n")), Lines));
		return FMessageDialog::Open(EAppMsgType::OkCancel, Message, LOCTEXT("ConfirmTitle", "Update Named Reroute Prefixes")) == EAppReturnType::Ok;
	}

	/** Every usage of the given declarations: renaming one reconstructs them, so they go in the undo step too. */
	TArray<UMaterialExpression*> UsagesOf(const UMaterialGraph* Graph, const TSet<UMaterialExpressionNamedRerouteDeclaration*>& Declarations)
	{
		TArray<UMaterialExpression*> Result;
		for (UMaterialExpression* E : VrrGraph::ExpressionsOf(Graph))
		{
			const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E);
			if (Usage && Declarations.Contains(Usage->Declaration.Get()))
			{
				Result.Add(E);
			}
		}
		return Result;
	}

	/** Toast suffix for reroutes an update couldn't type. */
	FText UntypedNote(int32 Untyped)
	{
		return Untyped
			? FText::Format(LOCTEXT("Untyped", " {0} reroute(s) whose type couldn't be told (no type prefix, no typed input) were left alone."), Untyped)
			: FText::GetEmpty();
	}

	// ---- Empty-space menu ----

	/** Runs an update on the graph; places nothing. */
	struct FUpdateAction : public FEdGraphSchemaAction
	{
		using FUpdate = void (*)(UMaterialGraph*);
		using FEdGraphSchemaAction::PerformAction;

		FUpdateAction(FUpdate InUpdate, FText Label, FText Tooltip, FText Keywords)
			: FEdGraphSchemaAction(LOCTEXT("Category", "Named Reroutes"), MoveTemp(Label), MoveTemp(Tooltip), 2 /* on top, like the engine's named reroutes */, MoveTemp(Keywords))
			, Update(InUpdate)
		{
		}

		virtual UEdGraphNode* PerformAction(UEdGraph* ParentGraph, UEdGraphPin* FromPin, const FVector2f& Location, bool bSelectNewNode) override
		{
			if (UMaterialGraph* Graph = Cast<UMaterialGraph>(ParentGraph))
			{
				Update(Graph);
			}
			return nullptr;
		}

		FUpdate Update;
	};
}

namespace VrrUpdateReroutes
{
	FCount CountColors(const UMaterialGraph* Graph)
	{
		const FColorPlan Plan = PlanColors(Graph);
		return { Plan.Declarations, Plan.Changes.Num() };
	}

	FCount CountPrefixes(const UMaterialGraph* Graph)
	{
		const FPrefixPlan Plan = PlanPrefixes(Graph);
		return { Plan.Declarations, Plan.Renames.Num() };
	}

	FText ColorsTooltip(const FCount& Count)
	{
		return Count.ToChange
			? FText::Format(LOCTEXT("ColorsTooltip",
				"Repaint {0} of the {1} named reroute(s) in this graph with the current color for their type (Project Settings > "
				"Plugins > Material Reroute Tools). The type comes from the name prefix, else from what feeds the reroute."),
				Count.ToChange, Count.Declarations)
			: FText::Format(LOCTEXT("ColorsTooltipDone",
				"All {0} named reroute(s) in this graph already have the current color for their type."), Count.Declarations);
	}

	FText PrefixesTooltip(const FCount& Count)
	{
		return Count.ToChange
			? FText::Format(LOCTEXT("PrefixesTooltip",
				"Rename {0} of the {1} named reroute(s) in this graph so their prefix matches the type they carry (Project "
				"Settings > Plugins > Material Reroute Tools): missing, old and wrong prefixes. The renames are listed first."),
				Count.ToChange, Count.Declarations)
			: FText::Format(LOCTEXT("PrefixesTooltipDone",
				"All {0} named reroute(s) in this graph have the right prefix for their type."), Count.Declarations);
	}

	void UpdateColors(UMaterialGraph* Graph)
	{
		const FColorPlan Plan = PlanColors(Graph);
		if (Plan.Changes.IsEmpty())
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("ColorsDone", "Named reroute colors are already up to date.{0}"), UntypedNote(Plan.Untyped)), true);
			return;
		}
		{
			const FScopedTransaction Transaction(LOCTEXT("ColorsTransaction", "Update Named Reroute Colors"));
			TArray<UMaterialExpressionNamedRerouteDeclaration*> Edited;
			for (const FRecolor& Change : Plan.Changes)
			{
				Change.Declaration->Modify();
				Change.Declaration->NodeColor = Change.Color;
				Edited.Add(Change.Declaration);
			}
			VrrGraph::RedrawAfterRerouteEdit(Graph, Edited);
		}
		VrrGraph::Notify(FText::Format(LOCTEXT("ColorsUpdated", "Updated the color of {0} named reroute(s).{1}"),
			Plan.Changes.Num(), UntypedNote(Plan.Untyped)), true);
	}

	void UpdatePrefixes(UMaterialGraph* Graph)
	{
		const FPrefixPlan Plan = PlanPrefixes(Graph);
		if (Plan.Renames.IsEmpty())
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("PrefixesDone", "Named reroute prefixes are already right.{0}"), UntypedNote(Plan.Untyped)), true);
			return;
		}
		if (!ConfirmRenames(Plan))	// the types come from the graph, so show what will change
		{
			return;
		}
		{
			const FScopedTransaction Transaction(LOCTEXT("PrefixesTransaction", "Update Named Reroute Prefixes"));
			TArray<UMaterialExpressionNamedRerouteDeclaration*> Edited;
			for (const FRename& Rename : Plan.Renames)
			{
				Rename.Declaration->Modify();
				Edited.Add(Rename.Declaration);
			}
			for (UMaterialExpression* Usage : UsagesOf(Graph, TSet<UMaterialExpressionNamedRerouteDeclaration*>(Edited)))
			{
				Usage->Modify();
			}

			// Free the old names first, so two reroutes swapping names ("s_A" <-> "vec3_A") don't get a "1" added
			for (const FRename& Rename : Plan.Renames)
			{
				Rename.Declaration->Name = *FString::Printf(TEXT("__VrrRename_%s"), *Rename.Declaration->VariableGuid.ToString());
			}
			for (const FRename& Rename : Plan.Renames)
			{
				UMaterialExpressionNamedRerouteDeclaration* Declaration = Rename.Declaration;
				Declaration->SetEditableName(Rename.NewName);	// made unique if taken; reconstructs its usages
				Declaration->NodeColor = VrrReroute::ColorForLink(Declaration->Input, Rename.Kind);
				if (Declaration->GraphNode)
				{
					Declaration->GraphNode->ReconstructNode();	// its own title
				}
			}
			VrrGraph::RedrawAfterRerouteEdit(Graph, Edited);
		}
		VrrGraph::Notify(FText::Format(LOCTEXT("PrefixesUpdated", "Renamed {0} named reroute(s).{1}"),
			Plan.Renames.Num(), UntypedNote(Plan.Untyped)), true);
	}
}

bool UVrrUpdateReroutesMenuNode::CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const
{
	return Schema && Schema->IsA<UMaterialGraphSchema>();
}

void UVrrUpdateReroutesMenuNode::GetMenuEntries(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	const UMaterialGraph* Graph = Cast<UMaterialGraph>(ContextMenuBuilder.CurrentGraph);
	if (!Graph || ContextMenuBuilder.FromPin)
	{
		return;	// only on empty graph space, not when dragging a wire
	}
	const VrrUpdateReroutes::FCount Colors = VrrUpdateReroutes::CountColors(Graph);
	if (Colors.Declarations == 0)
	{
		return;
	}
	ContextMenuBuilder.AddAction(MakeShared<FUpdateAction>(&VrrUpdateReroutes::UpdateColors,
		LOCTEXT("ColorsLabel", "Update Named Reroute Colors"), VrrUpdateReroutes::ColorsTooltip(Colors),
		LOCTEXT("ColorsKeywords", "named reroute color colour refresh standard")));
	ContextMenuBuilder.AddAction(MakeShared<FUpdateAction>(&VrrUpdateReroutes::UpdatePrefixes,
		LOCTEXT("PrefixesLabel", "Update Named Reroute Prefixes..."), VrrUpdateReroutes::PrefixesTooltip(VrrUpdateReroutes::CountPrefixes(Graph)),
		LOCTEXT("PrefixesKeywords", "named reroute prefix rename type check fix")));
}

#undef LOCTEXT_NAMESPACE
