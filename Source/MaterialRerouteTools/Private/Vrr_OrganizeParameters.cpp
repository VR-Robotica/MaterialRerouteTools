// Copyright VR Robotica. All Rights Reserved.

/*
 * Organize Parameters into Named Reroutes: gathers the selected material parameters into one column at the top left
 * of the graph and wires the rest of the material to them through named reroutes.
 *
 * What's organized: selected parameters (scalar, vector, static bool, texture object, texture sample, collection
 * parameter, ...) with no inputs wired, plus texture sample parameters (their UVs etc. stay wired). Other selected
 * nodes are ignored, so "select all" is fine. Static switches, channel mask parameters and the like sit in the middle
 * of the graph's logic and are left alone.
 *
 * Same-named value parameters (everything but texture sample parameters) are one parameter: the top-most stays, the
 * others' reads go through it, and they're deleted.
 *
 * Reroutes: one declaration per output pin the material reads (the main pin for a parameter nothing reads), named
 * "<prefix><Name>" and colored as in Vrr_RerouteStyle; a single channel is "<Name> R" or its custom channel name. A
 * declaration that already reads the pin is kept and moved into the column; extra ones for the same pin are merged
 * into it. Each read gets its own usage beside the reading node and keeps its channel mask.
 *
 * Layout: texture objects, texture samples, vectors, scalars, booleans, then anything else, each group top to bottom
 * as it was. The column goes below a column left by an earlier run (parameters that only feed named reroute
 * declarations), else left of everything else in the graph, level with its top.
 *
 * Steps
 *  1. Collect the selected parameters and map who reads what.
 *  2. Merge same-named parameters, then group each one's reads per output pin (existing declarations apart).
 *  3. Find where the column goes.
 *  4. Place the parameters and their declarations (new or reused), merging extra declarations.
 *  5. Replace every read with a usage, delete the merged duplicates.
 *  6. Rebuild the graph, select the column and report.
 */

#include "Vrr_OrganizeParameters.h"
#include "Vrr_MaterialGraphUtils.h"
#include "Vrr_RerouteStyle.h"

#include "IMaterialEditor.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "VrrOrganizeParameters"

namespace VrrOrganizeParameters
{
	namespace
	{
		using VrrGraph::FLink;
		using VrrReroute::EKind;

		// Column layout beyond VrrGraph::Layout, in graph units
		constexpr int32 DeclarationOffset = 420;	// parameter -> its declarations
		constexpr int32 ColumnWidth = 800;			// room the column takes left of the graph
		constexpr int32 RowGap = 32;
		constexpr int32 GroupGap = 96;
		using VrrGraph::Layout::DeclarationStep;

		/** Everything read from one output pin of an organized parameter. */
		struct FPinReads
		{
			int32 Pin = 0;
			TArray<FLink> Reads;													// become usages
			TArray<UMaterialExpressionNamedRerouteDeclaration*> Declarations;		// already read it whole
			UMaterialExpressionNamedRerouteDeclaration* Declaration = nullptr;		// the one kept or made
		};

		bool HasWiredInput(UMaterialExpression* E)
		{
			for (FExpressionInputIterator It{ E }; It; ++It)
			{
				if (It.Input->Expression)
				{
					return true;
				}
			}
			return false;
		}

		bool IsTextureObject(const UMaterialExpression* E) { return E->IsA<UMaterialExpressionTextureObjectParameter>(); }

		bool IsSamplerParameter(const UMaterialExpression* E) { return E->IsA<UMaterialExpressionTextureSampleParameter>() && !IsTextureObject(E); }

		bool IsOrganizable(UMaterialExpression* E)
		{
			return E && E->HasAParameterName() && !E->GetParameterName().IsNone() && (IsSamplerParameter(E) || !HasWiredInput(E));
		}

		/** A plain value: same-named copies are the same parameter and can be merged. */
		bool IsMergeable(UMaterialExpression* E)
		{
			return (E->IsA<UMaterialExpressionParameter>() || IsTextureObject(E)) && !HasWiredInput(E);
		}

		/** Column group: texture objects, texture samples, vectors, scalars, booleans, anything else. */
		int32 GroupOf(UMaterialExpression* E)
		{
			if (IsTextureObject(E))
			{
				return 0;
			}
			if (IsSamplerParameter(E))
			{
				return 1;
			}
			switch (VrrReroute::KindOf(E, 0))
			{
			case EKind::Vector2:
			case EKind::Vector3:
			case EKind::Vector4: return 2;
			case EKind::Scalar:  return 3;
			case EKind::Bool:    return 4;
			default:             return 5;
			}
		}

		/** Rough node height per group. */
		int32 EstimatedHeight(UMaterialExpression* E)
		{
			static const int32 Heights[] = { 260, 300, 200, 64, 64, 140 };
			return Heights[GroupOf(E)];
		}

		/** Reroute name (without prefix) for one pin: the parameter's name, or "<Name> R" / the custom channel name. */
		FString NameForPin(UMaterialExpression* E, int32 Pin)
		{
			const FString Base = E->GetParameterName().ToString();
			TArray<FExpressionOutput>& Outputs = E->GetOutputs();
			if (Outputs.Num() <= 1 || !Outputs.IsValidIndex(Pin))
			{
				return Base;
			}
			const FExpressionOutput& Output = Outputs[Pin];
			const int32 Channel = VrrGraph::SingleChannelOf(Output);
			if (Channel != INDEX_NONE)
			{
				return VrrGraph::ChannelName(E, Base, Channel);
			}
			if (VrrGraph::MaskWidth(Output) >= 3 || Pin == 0 || Output.OutputName.IsNone())
			{
				return Base;	// RGB / RGBA: the prefix tells them apart
			}
			return FString::Printf(TEXT("%s %s"), *Base, *Output.OutputName.ToString());
		}

		/** A declaration that reads a whole pin (no mask), so it can stand for that pin. */
		UMaterialExpressionNamedRerouteDeclaration* AsPlainDeclaration(const FLink& Link)
		{
			UMaterialExpressionNamedRerouteDeclaration* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Link.Consumer);
			return Declaration && Link.Input == &Declaration->Input && !Link.Input->Mask ? Declaration : nullptr;
		}

		/** Moves Extra's usages over to Kept, then deletes Extra. */
		void MergeDeclaration(UMaterial* Material, UMaterialExpressionNamedRerouteDeclaration* Extra, UMaterialExpressionNamedRerouteDeclaration* Kept)
		{
			for (UMaterialExpression* E : Material->GetExpressions())
			{
				UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E);
				if (Usage && Usage->Declaration == Extra)
				{
					Usage->Modify();
					VrrGraph::BindUsage(Usage, Kept);
				}
			}
			Extra->Modify();
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Extra);
		}
	}

	TArray<UMaterialExpression*> GetSelectedParameters(const UMaterialGraph* Graph)
	{
		return VrrGraph::GetSelectedExpressions(Graph, IsOrganizable);
	}

	void OrganizeSelection(UMaterialGraph* Graph)
	{
		UMaterial* Material = Graph ? Graph->Material.Get() : nullptr;
		const TSharedPtr<IMaterialEditor> Editor = VrrGraph::GetEditor(Graph);
		if (!Material || !Editor.IsValid() || Graph->MaterialFunction)
		{
			return;
		}
		const FText Tool = LOCTEXT("Tool", "Organize Parameters into Named Reroutes");

		// 1. Selection (top to bottom) and who reads what
		TArray<UMaterialExpression*> Selected = GetSelectedParameters(Graph);
		if (Selected.IsEmpty())
		{
			VrrGraph::Notify(FText::Format(LOCTEXT("NothingSelected", "{0}: select parameters first."), Tool), false);
			return;
		}
		Selected.Sort(VrrGraph::ByPosition);
		const VrrGraph::FReaders Readers = VrrGraph::MapReaders(Material);

		// 2. Same-named value parameters merge into the top-most one
		TMap<TPair<UClass*, FName>, UMaterialExpression*> FirstByName;
		TMap<UMaterialExpression*, UMaterialExpression*> CanonicalOf;
		TArray<UMaterialExpression*> Parameters;
		TArray<UMaterialExpression*> Duplicates;
		for (UMaterialExpression* E : Selected)
		{
			if (IsMergeable(E))
			{
				const TPair<UClass*, FName> Key(E->GetClass(), E->GetParameterName());
				if (UMaterialExpression** First = FirstByName.Find(Key))
				{
					CanonicalOf.Add(E, *First);
					Duplicates.Add(E);
					continue;
				}
				FirstByName.Add(Key, E);
			}
			CanonicalOf.Add(E, E);
			Parameters.Add(E);
		}

		// 2a. Reads per output pin; declarations that read a pin whole are kept apart (reused, not given usages)
		TMap<UMaterialExpression*, TArray<FPinReads>> PinsOf;
		TSet<UMaterialExpression*> Moving(Selected);	// everything that moves or goes away, left out of the graph bounds
		for (UMaterialExpression* E : Selected)
		{
			TArray<FPinReads>& Pins = PinsOf.FindOrAdd(CanonicalOf[E]);
			const TArray<FLink>* Reads = Readers.Find(E);
			if (!Reads)
			{
				continue;
			}
			for (const FLink& Link : *Reads)
			{
				const int32 Pin = Link.Input->OutputIndex;
				FPinReads* PinReads = Pins.FindByPredicate([Pin](const FPinReads& P) { return P.Pin == Pin; });
				if (!PinReads)
				{
					PinReads = &Pins.AddDefaulted_GetRef();
					PinReads->Pin = Pin;
				}
				if (UMaterialExpressionNamedRerouteDeclaration* Declaration = AsPlainDeclaration(Link))
				{
					PinReads->Declarations.AddUnique(Declaration);
					Moving.Add(Declaration);
				}
				else
				{
					PinReads->Reads.Add(Link);
				}
			}
		}
		for (UMaterialExpression* E : Parameters)
		{
			TArray<FPinReads>& Pins = PinsOf[E];
			if (Pins.IsEmpty())
			{
				Pins.AddDefaulted();	// read by nothing: a declaration on its main pin, ready to use
			}
			Pins.Sort([](const FPinReads& A, const FPinReads& B) { return A.Pin < B.Pin; });
		}

		// 3. Where the column goes: below an earlier run's column (a parameter that only feeds named reroute
		//    declarations), else left of the rest of the graph, level with its top
		bool bEarlierColumn = false;
		int32 ColumnX = 0;
		int32 ColumnBottom = 0;
		int32 MinX = Material->EditorX;
		int32 MinY = Material->EditorY;
		for (UMaterialExpression* E : Material->GetExpressions())
		{
			if (!E || Moving.Contains(E))
			{
				continue;
			}
			MinX = FMath::Min(MinX, E->MaterialExpressionEditorX);
			MinY = FMath::Min(MinY, E->MaterialExpressionEditorY);

			const TArray<FLink>* Reads = Readers.Find(E);
			if (IsOrganizable(E) && Reads && !Reads->IsEmpty()
				&& !Reads->ContainsByPredicate([](const FLink& L) { return !AsPlainDeclaration(L); }))
			{
				int32 Bottom = E->MaterialExpressionEditorY + EstimatedHeight(E);
				for (const FLink& L : *Reads)
				{
					Bottom = FMath::Max(Bottom, L.Consumer->MaterialExpressionEditorY + DeclarationStep);
				}
				ColumnX = bEarlierColumn ? FMath::Min(ColumnX, E->MaterialExpressionEditorX) : E->MaterialExpressionEditorX;
				ColumnBottom = bEarlierColumn ? FMath::Max(ColumnBottom, Bottom) : Bottom;
				bEarlierColumn = true;
			}
		}
		for (const UMaterialExpressionComment* Comment : Material->GetEditorComments())
		{
			if (Comment)
			{
				MinX = FMath::Min(MinX, Comment->MaterialExpressionEditorX);
				MinY = FMath::Min(MinY, Comment->MaterialExpressionEditorY);
			}
		}
		const int32 X = bEarlierColumn ? ColumnX : MinX - ColumnWidth;
		int32 Y = bEarlierColumn ? ColumnBottom + GroupGap : MinY;

		const FScopedTransaction Transaction(Tool);
		Material->Modify();

		// 4. Parameters and their declarations, by group
		Parameters.StableSort([](UMaterialExpression& A, UMaterialExpression& B) { return GroupOf(&A) < GroupOf(&B); });
		TArray<const UMaterialExpression*> Column;	// selected at the end
		int32 NewDeclarations = 0;
		int32 ReusedDeclarations = 0;
		int32 PreviousGroup = INDEX_NONE;
		for (UMaterialExpression* E : Parameters)
		{
			const int32 Group = GroupOf(E);
			if (PreviousGroup != INDEX_NONE && Group != PreviousGroup)
			{
				Y += GroupGap - RowGap;
			}
			PreviousGroup = Group;

			E->Modify();
			E->MaterialExpressionEditorX = X;
			E->MaterialExpressionEditorY = Y;
			Column.Add(E);
			int32 DeclarationY = Y;
			for (FPinReads& Pin : PinsOf[E])
			{
				if (!Pin.Declarations.IsEmpty())
				{
					// 4a. Keep the first declaration, now reading this node (it may have read a merged duplicate)
					Pin.Declaration = Pin.Declarations[0];
					Pin.Declaration->Modify();
					Pin.Declaration->Input.Expression = E;
					Pin.Declaration->MaterialExpressionEditorX = X + DeclarationOffset;
					Pin.Declaration->MaterialExpressionEditorY = DeclarationY;
					++ReusedDeclarations;
					for (int32 Index = 1; Index < Pin.Declarations.Num(); ++Index)
					{
						MergeDeclaration(Material, Pin.Declarations[Index], Pin.Declaration);
					}
				}
				else
				{
					const EKind Kind = VrrReroute::KindOf(E, Pin.Pin);
					Pin.Declaration = VrrGraph::CreateDeclaration(Material, E, Pin.Pin, X + DeclarationOffset, DeclarationY,
						FString::Printf(TEXT("%s%s"), VrrReroute::Prefix(Kind), *NameForPin(E, Pin.Pin)), VrrReroute::ColorFor(E, Pin.Pin, Kind));
					if (!Pin.Declaration)
					{
						continue;
					}
					++NewDeclarations;
				}
				Column.Add(Pin.Declaration);
				DeclarationY += DeclarationStep;
			}
			Y = FMath::Max(Y + EstimatedHeight(E), DeclarationY) + RowGap;
		}

		// 5. A usage beside every read (reads keep their channel mask)
		int32 Usages = 0;
		TMap<UMaterialExpression*, int32> UsagesPerConsumer;
		for (UMaterialExpression* E : Parameters)
		{
			for (const FPinReads& Pin : PinsOf[E])
			{
				if (!Pin.Declaration)
				{
					continue;
				}
				for (const FLink& Read : Pin.Reads)
				{
					const int32 Slot = UsagesPerConsumer.FindOrAdd(Read.Consumer)++;
					if (UMaterialExpressionNamedRerouteUsage* Usage = VrrGraph::CreateUsage(Material, Pin.Declaration, VrrGraph::UsagePositionFor(Material, Read.Consumer, Slot)))
					{
						VrrGraph::Rewire(Read, Usage);
						++Usages;
					}
				}
			}
		}

		// 5a. Merged duplicates are read through their kept parameter now
		for (UMaterialExpression* Duplicate : Duplicates)
		{
			Duplicate->Modify();
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Duplicate);
		}

		// 6. Show it, select the column so it moves as one block, and report
		VrrGraph::FinishEdit(Graph, *Editor, Column);
		FText Message = FText::Format(LOCTEXT("Done", "Organized {0} parameter(s): {1} new named reroute(s), {2} reused, {3} usage(s)."),
			Parameters.Num(), NewDeclarations, ReusedDeclarations, Usages);
		if (!Duplicates.IsEmpty())
		{
			Message = FText::Format(LOCTEXT("DoneMerged", "{0}\nMerged {1} duplicate parameter node(s)."), Message, Duplicates.Num());
		}
		VrrGraph::Notify(Message, true);
	}
}

#undef LOCTEXT_NAMESPACE
