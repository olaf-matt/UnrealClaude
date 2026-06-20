// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Widget.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"

#if WITH_EDITOR
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/Field.h"
#endif

#if WITH_EDITOR
namespace
{
	/** Load a Widget Blueprint by asset path (with/without _C, tolerant of .uasset object path form). */
	UWidgetBlueprint* LoadWidgetBlueprint(const FString& Path)
	{
		// Strip a trailing ".Object" form if present; LoadObject handles both.
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *Path);
		if (!WBP)
		{
			// Try the package.object form: /Game/Foo/WBP_X -> /Game/Foo/WBP_X.WBP_X
			FString ObjPath = Path;
			int32 Dot;
			if (!ObjPath.FindChar(TEXT('.'), Dot))
			{
				FString Leaf;
				if (ObjPath.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
				{
					ObjPath = Path + TEXT(".") + Leaf;
					WBP = LoadObject<UWidgetBlueprint>(nullptr, *ObjPath);
				}
			}
		}
		return WBP;
	}

	/** Resolve a widget class from a friendly name (e.g. "Slider"), a /Script/UMG path, or a /Game BP path. */
	UClass* ResolveWidgetClass(const FString& In)
	{
		if (In.IsEmpty())
		{
			return nullptr;
		}

		// /Game/... user widget asset -> generated class
		if (In.StartsWith(TEXT("/Game/")))
		{
			if (UClass* C = LoadClass<UWidget>(nullptr, *In))
			{
				return C;
			}
			if (!In.EndsWith(TEXT("_C")))
			{
				if (UClass* C = LoadClass<UWidget>(nullptr, *(In + TEXT("_C"))))
				{
					return C;
				}
			}
		}

		// Full script path given directly
		if (In.StartsWith(TEXT("/Script/")))
		{
			if (UClass* C = LoadClass<UWidget>(nullptr, *In))
			{
				return C;
			}
		}

		// Friendly name -> /Script/UMG.<Name>
		if (UClass* C = LoadClass<UWidget>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *In)))
		{
			return C;
		}
		// Tolerate a leading "U" (e.g. "USlider")
		if (In.StartsWith(TEXT("U")) && In.Len() > 1)
		{
			if (UClass* C = LoadClass<UWidget>(nullptr, *FString::Printf(TEXT("/Script/UMG.%s"), *In.RightChop(1))))
			{
				return C;
			}
		}
		return nullptr;
	}

	UWidget* FindWidgetByName(UWidgetTree* Tree, const FString& Name)
	{
		if (!Tree)
		{
			return nullptr;
		}
		UWidget* Found = nullptr;
		const FName Target(*Name);
		Tree->ForEachWidget([&Found, &Target](UWidget* W)
		{
			if (W && !Found && W->GetFName() == Target)
			{
				Found = W;
			}
		});
		return Found;
	}

	FString JsonValueToString(const TSharedPtr<FJsonValue>& Val)
	{
		if (!Val.IsValid())
		{
			return FString();
		}
		switch (Val->Type)
		{
		case EJson::String:  return Val->AsString();
		case EJson::Boolean: return Val->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
		{
			const double D = Val->AsNumber();
			return (D == FMath::TruncToDouble(D))
				? FString::Printf(TEXT("%lld"), (int64)D)
				: FString::SanitizeFloat(D);
		}
		default: return Val->AsString();
		}
	}

	/** Set a property on a widget by name. Handles common scalar types explicitly, ImportText fallback. */
	bool SetWidgetPropByName(UWidget* W, const FString& PropName,
		const TSharedPtr<FJsonValue>& Val, FString& OutError)
	{
		FProperty* Prop = W->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropName, *W->GetClass()->GetName());
			return false;
		}
		void* Addr = Prop->ContainerPtrToValuePtr<void>(W);

		if (FTextProperty* TP = CastField<FTextProperty>(Prop))
		{
			TP->SetPropertyValue(Addr, FText::FromString(JsonValueToString(Val)));
			return true;
		}
		if (FStrProperty* SP = CastField<FStrProperty>(Prop))
		{
			SP->SetPropertyValue(Addr, JsonValueToString(Val));
			return true;
		}
		if (FNameProperty* NP = CastField<FNameProperty>(Prop))
		{
			NP->SetPropertyValue(Addr, FName(*JsonValueToString(Val)));
			return true;
		}
		if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		{
			const bool B = (Val.IsValid() && Val->Type == EJson::Boolean) ? Val->AsBool()
				: JsonValueToString(Val).ToBool();
			BP->SetPropertyValue(Addr, B);
			return true;
		}
		const bool bNumeric = Val.IsValid() && Val->Type == EJson::Number;
		if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
		{
			FP->SetPropertyValue(Addr, bNumeric ? (float)Val->AsNumber() : FCString::Atof(*JsonValueToString(Val)));
			return true;
		}
		if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
		{
			DP->SetPropertyValue(Addr, bNumeric ? Val->AsNumber() : FCString::Atod(*JsonValueToString(Val)));
			return true;
		}
		if (FIntProperty* IP = CastField<FIntProperty>(Prop))
		{
			IP->SetPropertyValue(Addr, bNumeric ? (int32)Val->AsNumber() : FCString::Atoi(*JsonValueToString(Val)));
			return true;
		}

		// Fallback: import from string (structs, enums, colors as text)
		const FString S = JsonValueToString(Val);
		if (!Prop->ImportText_Direct(*S, Addr, W, PPF_None))
		{
			OutError = FString::Printf(TEXT("Could not set property '%s' (type %s) from value '%s'"),
				*PropName, *Prop->GetCPPType(), *S);
			return false;
		}
		return true;
	}

	/** Export the edit-visible scalar/string properties of a widget for read-back. */
	void ExportEditProperties(UWidget* W, const TSharedPtr<FJsonObject>& Out)
	{
		for (TFieldIterator<FProperty> It(W->GetClass()); It; ++It)
		{
			FProperty* P = *It;
			if (!P->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			const void* Addr = P->ContainerPtrToValuePtr<void>(W);
			const FString PName = P->GetName();

			if (FBoolProperty* BP = CastField<FBoolProperty>(P))
			{
				Out->SetBoolField(PName, BP->GetPropertyValue(Addr));
			}
			else if (FFloatProperty* FP = CastField<FFloatProperty>(P))
			{
				Out->SetNumberField(PName, FP->GetPropertyValue(Addr));
			}
			else if (FDoubleProperty* DP = CastField<FDoubleProperty>(P))
			{
				Out->SetNumberField(PName, DP->GetPropertyValue(Addr));
			}
			else if (FIntProperty* IP = CastField<FIntProperty>(P))
			{
				Out->SetNumberField(PName, IP->GetPropertyValue(Addr));
			}
			else if (FTextProperty* TP = CastField<FTextProperty>(P))
			{
				const FText T = TP->GetPropertyValue(Addr);
				if (!T.IsEmpty())
				{
					Out->SetStringField(PName, T.ToString());
				}
			}
			else if (FStrProperty* SP = CastField<FStrProperty>(P))
			{
				const FString S = SP->GetPropertyValue(Addr);
				if (!S.IsEmpty())
				{
					Out->SetStringField(PName, S);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> BuildWidgetJson(UWidget* W)
	{
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), W->GetName());
		J->SetStringField(TEXT("class"), W->GetClass()->GetName());
		J->SetBoolField(TEXT("is_variable"), W->bIsVariable);

		if (UPanelWidget* Parent = W->GetParent())
		{
			J->SetStringField(TEXT("parent"), Parent->GetName());
		}
		else
		{
			J->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
		}

		if (W->Slot)
		{
			J->SetStringField(TEXT("slot_class"), W->Slot->GetClass()->GetName());
		}
		if (UPanelWidget* AsPanel = Cast<UPanelWidget>(W))
		{
			J->SetNumberField(TEXT("child_count"), AsPanel->GetChildrenCount());
		}

		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		ExportEditProperties(W, Props);
		J->SetObjectField(TEXT("properties"), Props);
		return J;
	}

	void FinalizeBlueprintEdit(UWidgetBlueprint* WBP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		FKismetEditorUtilities::CompileBlueprint(WBP);
		WBP->MarkPackageDirty();
	}

	/** Collect the names of every BlueprintAssignable multicast delegate on a widget class. */
	TArray<FString> ListWidgetDelegates(UClass* WidgetClass)
	{
		TArray<FString> Names;
		for (TFieldIterator<FMulticastDelegateProperty> It(WidgetClass); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				Names.Add(It->GetName());
			}
		}
		return Names;
	}

	/** Export a node's output pins (skipping the hidden delegate self pin) for read-back. */
	TArray<TSharedPtr<FJsonValue>> ExportOutputPins(const UEdGraphNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Pins;
		if (!Node)
		{
			return Pins;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->bHidden)
			{
				continue;
			}
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Pin->PinName.ToString());
			FString Type = Pin->PinType.PinCategory.ToString();
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				Type += TEXT(":") + Pin->PinType.PinSubCategoryObject->GetName();
			}
			P->SetStringField(TEXT("type"), Type);
			Pins.Add(MakeShared<FJsonValueObject>(P));
		}
		return Pins;
	}
}
#endif // WITH_EDITOR

FMCPToolInfo FMCPTool_Widget::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("widget");
	Info.Description = TEXT(
		"Author a Widget Blueprint's widget tree (the UMG Designer) — add/remove/arrange widgets,\n"
		"set their properties, and READ THE TREE BACK. Complements blueprint_modify, which wires the\n"
		"widget event graph/bindings but cannot touch the Designer tree.\n\n"
		"OPERATION → REQUIRED PARAMS:\n"
		"  get_widget_tree     → blueprint_path\n"
		"  add_widget          → blueprint_path, widget_class, widget_name [, parent, is_variable,\n"
		"                        properties, position, size]\n"
		"  remove_widget       → blueprint_path, widget_name\n"
		"  set_widget_property → blueprint_path, widget_name, property, value\n"
		"  add_widget_event    → blueprint_path, widget_name, event_name\n\n"
		"add_widget_event creates the widget bound-event node (K2Node_ComponentBoundEvent) in the\n"
		"event graph — the node blueprint_modify's add_node CANNOT make (e.g. a Slider's\n"
		"'OnValueChanged', a Button's 'OnClicked', a CheckBox's 'OnCheckStateChanged'). event_name is\n"
		"the widget's delegate property name. The widget is forced to a variable first (required for a\n"
		"bound event) and the BP recompiled. Idempotent: if the node already exists it is returned, not\n"
		"duplicated. Returns node_guid + output_pins[] (the delegate's exec + data outputs, e.g. a\n"
		"Slider's float 'Value') so blueprint_modify can wire downstream logic to that node by guid. An\n"
		"unknown event_name returns the list of the widget's available delegates.\n\n"
		"widget_class: friendly name resolved as /Script/UMG.<name> — e.g. 'Slider','TextBlock',\n"
		"'Button','CheckBox','EditableTextBox','ProgressBar','Image','Border','SizeBox','VerticalBox',\n"
		"'HorizontalBox','CanvasPanel','Overlay','ScrollBox','UniformGridPanel'. A /Game/... path loads\n"
		"a user-widget's generated class. parent = a panel widget's name, or 'root' (default): if the\n"
		"tree has no root the new widget BECOMES the root; otherwise it is added under the named panel\n"
		"(or the root panel). is_variable (default true) exposes the widget as a Blueprint variable\n"
		"after recompile so the event graph/bindings can reference it.\n\n"
		"properties: object of {PropertyName: value} applied to the new widget (e.g. Slider\n"
		"{\"MinValue\":0,\"MaxValue\":30,\"Value\":5,\"StepSize\":0.5}, TextBlock {\"Text\":\"Wind\"}).\n"
		"position/size: {\"x\":..,\"y\":..} applied to a CanvasPanelSlot when the parent is a CanvasPanel.\n\n"
		"get_widget_tree returns root (name) + widgets[]: each has name, class, is_variable, parent,\n"
		"slot_class, child_count (panels), and a properties{} map of edit-visible numeric/bool/string\n"
		"values (Slider MinValue/MaxValue/Value/StepSize, TextBlock Text, etc.) for verification.\n\n"
		"All write ops recompile the Blueprint so the generated class reflects the change. Iterate on\n"
		"/Game/TestCases/ duplicates, not production widgets."
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("get_widget_tree | add_widget | remove_widget | set_widget_property | add_widget_event"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("blueprint_path"), TEXT("string"),
		TEXT("Full asset path to the Widget Blueprint (e.g. '/Game/UI/WBP_HUD'). Required."), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("widget_class"), TEXT("string"),
		TEXT("Widget class friendly name (e.g. 'Slider','TextBlock','CanvasPanel') or /Script/UMG or /Game path. For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("widget_name"), TEXT("string"),
		TEXT("Name of the widget to create/remove/edit. For add_widget/remove_widget/set_widget_property.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("parent"), TEXT("string"),
		TEXT("Parent panel widget name, or 'root' (default). For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("is_variable"), TEXT("boolean"),
		TEXT("Expose the new widget as a Blueprint variable (default true). For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("properties"), TEXT("object"),
		TEXT("{PropertyName: value} to apply to the new widget. For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("position"), TEXT("object"),
		TEXT("{\"x\":..,\"y\":..} CanvasPanelSlot position (when parent is a CanvasPanel). For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("size"), TEXT("object"),
		TEXT("{\"x\":..,\"y\":..} CanvasPanelSlot size (when parent is a CanvasPanel). For add_widget.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("property"), TEXT("string"),
		TEXT("Property name to set. For set_widget_property.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("value"), TEXT("any"),
		TEXT("New property value (number/bool/string). For set_widget_property.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("event_name"), TEXT("string"),
		TEXT("Widget delegate name to bind, e.g. 'OnValueChanged' (Slider), 'OnClicked' (Button), 'OnCheckStateChanged' (CheckBox). For add_widget_event.")));

	Info.Annotations = FMCPToolAnnotations::Destructive();
	return Info;
}

FMCPToolResult FMCPTool_Widget::Execute(const TSharedRef<FJsonObject>& Params)
{
#if !WITH_EDITOR
	return FMCPToolResult::Error(TEXT("widget tool requires the editor (WITH_EDITOR)."));
#else
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	if (Operation == TEXT("get_widget_tree"))   return ExecuteGetWidgetTree(Params);
	if (Operation == TEXT("add_widget"))        return ExecuteAddWidget(Params);
	if (Operation == TEXT("remove_widget"))     return ExecuteRemoveWidget(Params);
	if (Operation == TEXT("set_widget_property")) return ExecuteSetWidgetProperty(Params);
	if (Operation == TEXT("add_widget_event"))   return ExecuteAddWidgetEvent(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation '%s'. Valid: get_widget_tree, add_widget, remove_widget, set_widget_property, add_widget_event."),
		*Operation));
#endif
}

#if WITH_EDITOR

FMCPToolResult FMCPTool_Widget::ExecuteGetWidgetTree(const TSharedRef<FJsonObject>& Params)
{
	FString Path;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), Path, Error))
	{
		return Error.GetValue();
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprint(Path);
	if (!WBP)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load Widget Blueprint: %s"), *Path));
	}
	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Widget Blueprint '%s' has no WidgetTree."), *Path));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), Path);
	if (Tree->RootWidget)
	{
		Data->SetStringField(TEXT("root"), Tree->RootWidget->GetName());
	}
	else
	{
		Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
	}

	TArray<TSharedPtr<FJsonValue>> WidgetArray;
	Tree->ForEachWidget([&WidgetArray](UWidget* W)
	{
		if (W)
		{
			WidgetArray.Add(MakeShared<FJsonValueObject>(BuildWidgetJson(W)));
		}
	});
	Data->SetArrayField(TEXT("widgets"), WidgetArray);
	Data->SetNumberField(TEXT("count"), WidgetArray.Num());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Widget tree for '%s': %d widget(s)."), *WBP->GetName(), WidgetArray.Num()),
		Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteAddWidget(const TSharedRef<FJsonObject>& Params)
{
	FString Path, ClassName, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_class"), ClassName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	UWidgetBlueprint* WBP = LoadWidgetBlueprint(Path);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load Widget Blueprint or its tree: %s"), *Path));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UClass* WidgetClass = ResolveWidgetClass(ClassName);
	if (!WidgetClass)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not resolve widget class '%s' (try a friendly UMG name like 'Slider' or 'TextBlock')."), *ClassName));
	}

	if (FindWidgetByName(Tree, WidgetName))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("A widget named '%s' already exists in '%s'."), *WidgetName, *WBP->GetName()));
	}

	WBP->Modify();
	Tree->Modify();

	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!NewWidget)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to construct widget of class '%s'."), *ClassName));
	}

	// Parent resolution
	const FString ParentName = ExtractOptionalString(Params, TEXT("parent"), TEXT("root"));
	FString SlotInfo;
	UPanelSlot* CreatedSlot = nullptr;

	if (!Tree->RootWidget)
	{
		// Empty tree -> this widget becomes the root
		Tree->RootWidget = NewWidget;
		SlotInfo = TEXT("root");
	}
	else
	{
		UPanelWidget* ParentPanel = nullptr;
		if (ParentName.IsEmpty() || ParentName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
		{
			ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
			if (!ParentPanel)
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Root widget '%s' is not a panel; specify a panel 'parent' to hold '%s'."),
					*Tree->RootWidget->GetName(), *WidgetName));
			}
		}
		else
		{
			UWidget* Found = FindWidgetByName(Tree, ParentName);
			if (!Found)
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Parent widget '%s' not found."), *ParentName));
			}
			ParentPanel = Cast<UPanelWidget>(Found);
			if (!ParentPanel)
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Parent widget '%s' (%s) is not a panel and cannot hold children."),
					*ParentName, *Found->GetClass()->GetName()));
			}
		}

		CreatedSlot = ParentPanel->AddChild(NewWidget);
		if (!CreatedSlot)
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Parent panel '%s' rejected the child (it may not accept multiple children)."),
				*ParentPanel->GetName()));
		}
		SlotInfo = CreatedSlot->GetClass()->GetName();
	}

	// is_variable (default true) so the event graph/bindings can reference it post-compile
	NewWidget->bIsVariable = ExtractOptionalBool(Params, TEXT("is_variable"), true);

	// Initial properties
	TArray<FString> PropWarnings;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			FString PropErr;
			if (!SetWidgetPropByName(NewWidget, Pair.Key, Pair.Value, PropErr))
			{
				PropWarnings.Add(PropErr);
			}
		}
	}

	// CanvasPanelSlot position/size
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(CreatedSlot))
	{
		const TSharedPtr<FJsonObject>* PosObj = nullptr;
		if (Params->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
		{
			double X = 0.0, Y = 0.0;
			(*PosObj)->TryGetNumberField(TEXT("x"), X);
			(*PosObj)->TryGetNumberField(TEXT("y"), Y);
			CanvasSlot->SetPosition(FVector2D(X, Y));
		}
		const TSharedPtr<FJsonObject>* SizeObj = nullptr;
		if (Params->TryGetObjectField(TEXT("size"), SizeObj) && SizeObj && (*SizeObj).IsValid())
		{
			double X = 0.0, Y = 0.0;
			(*SizeObj)->TryGetNumberField(TEXT("x"), X);
			(*SizeObj)->TryGetNumberField(TEXT("y"), Y);
			CanvasSlot->SetSize(FVector2D(X, Y));
		}
	}

	FinalizeBlueprintEdit(WBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("add_widget"));
	Data->SetStringField(TEXT("widget_name"), NewWidget->GetName());
	Data->SetStringField(TEXT("widget_class"), NewWidget->GetClass()->GetName());
	Data->SetBoolField(TEXT("is_variable"), NewWidget->bIsVariable);
	Data->SetStringField(TEXT("slot_type"), SlotInfo);
	if (PropWarnings.Num() > 0)
	{
		Data->SetArrayField(TEXT("property_warnings"), StringArrayToJsonArray(PropWarnings));
	}

	FMCPToolResult Result = FMCPToolResult::Success(
		FString::Printf(TEXT("Added %s '%s' to '%s' (slot: %s)."),
			*NewWidget->GetClass()->GetName(), *NewWidget->GetName(), *WBP->GetName(), *SlotInfo),
		Data);
	Result.Warnings = PropWarnings;
	return Result;
}

FMCPToolResult FMCPTool_Widget::ExecuteRemoveWidget(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();

	UWidgetBlueprint* WBP = LoadWidgetBlueprint(Path);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load Widget Blueprint or its tree: %s"), *Path));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = FindWidgetByName(Tree, WidgetName);
	if (!Widget)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *WBP->GetName()));
	}

	WBP->Modify();
	Tree->Modify();

	const bool bRemoved = Tree->RemoveWidget(Widget);
	if (!bRemoved)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to remove widget '%s'."), *WidgetName));
	}

	FinalizeBlueprintEdit(WBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("remove_widget"));
	Data->SetStringField(TEXT("widget_name"), WidgetName);
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Removed widget '%s' from '%s'."), *WidgetName, *WBP->GetName()), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteSetWidgetProperty(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName, PropName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("property"), PropName, Error)) return Error.GetValue();

	const TSharedPtr<FJsonValue> Value = Params->Values.FindRef(TEXT("value"));
	if (!Value.IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: value"));
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprint(Path);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load Widget Blueprint or its tree: %s"), *Path));
	}

	UWidget* Widget = FindWidgetByName(WBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *WBP->GetName()));
	}

	WBP->Modify();

	FString PropErr;
	if (!SetWidgetPropByName(Widget, PropName, Value, PropErr))
	{
		return FMCPToolResult::Error(PropErr);
	}

	FinalizeBlueprintEdit(WBP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("set_widget_property"));
	Data->SetStringField(TEXT("widget_name"), WidgetName);
	Data->SetStringField(TEXT("property"), PropName);
	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %s.%s on '%s'."), *WidgetName, *PropName, *WBP->GetName()), Data);
}

FMCPToolResult FMCPTool_Widget::ExecuteAddWidgetEvent(const TSharedRef<FJsonObject>& Params)
{
	FString Path, WidgetName, EventName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("blueprint_path"), Path, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("widget_name"), WidgetName, Error)) return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("event_name"), EventName, Error)) return Error.GetValue();

	UWidgetBlueprint* WBP = LoadWidgetBlueprint(Path);
	if (!WBP || !WBP->WidgetTree)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Could not load Widget Blueprint or its tree: %s"), *Path));
	}

	UWidget* Widget = FindWidgetByName(WBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *WBP->GetName()));
	}
	UClass* WidgetClass = Widget->GetClass();

	// The delegate must exist on the widget class and be BlueprintAssignable.
	FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(WidgetClass, FName(*EventName));
	if (!Delegate || !Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable))
	{
		const TArray<FString> Available = ListWidgetDelegates(WidgetClass);
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("available_events"), StringArrayToJsonArray(Available));
		FMCPToolResult Result = FMCPToolResult::Error(FString::Printf(
			TEXT("Event '%s' is not a bindable delegate on %s. Available: %s"),
			*EventName, *WidgetClass->GetName(),
			Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
		Result.Data = Data;
		return Result;
	}
	// Use the delegate's canonical name from here on.
	const FName EventFName = Delegate->GetFName();

	// A bound event requires the widget to be a Blueprint variable; force it and recompile so the
	// FObjectProperty exists on the (skeleton) generated class.
	bool bForcedVariable = false;
	if (!Widget->bIsVariable)
	{
		WBP->Modify();
		Widget->bIsVariable = true;
		bForcedVariable = true;
		FinalizeBlueprintEdit(WBP);
	}

	// Locate the widget variable property on the skeleton class (matches the editor's "+" path).
	FObjectProperty* VarProp = FindFProperty<FObjectProperty>(WBP->SkeletonGeneratedClass, FName(*WidgetName));
	if (!VarProp)
	{
		// One more compile in case the skeleton wasn't refreshed yet.
		FinalizeBlueprintEdit(WBP);
		VarProp = FindFProperty<FObjectProperty>(WBP->SkeletonGeneratedClass, FName(*WidgetName));
	}
	if (!VarProp)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Could not resolve widget variable property '%s' on the generated class (widget is_variable=%s)."),
			*WidgetName, Widget->bIsVariable ? TEXT("true") : TEXT("false")));
	}

	// Idempotent: reuse an existing bound-event node if present.
	const UK2Node_ComponentBoundEvent* Existing =
		FKismetEditorUtilities::FindBoundEventForComponent(WBP, EventFName, VarProp->GetFName());
	bool bCreated = false;
	if (!Existing)
	{
		WBP->Modify();
		FKismetEditorUtilities::CreateNewBoundEventForClass(WidgetClass, EventFName, WBP, VarProp);
		FinalizeBlueprintEdit(WBP);
		bCreated = true;
	}

	// Read-back: find the node and report its guid + delegate output pins.
	const UK2Node_ComponentBoundEvent* Node =
		FKismetEditorUtilities::FindBoundEventForComponent(WBP, EventFName, VarProp->GetFName());
	if (!Node)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Bound event for %s.%s was not found after creation (creation may have failed)."),
			*WidgetName, *EventFName.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("operation"), TEXT("add_widget_event"));
	Data->SetStringField(TEXT("widget_name"), WidgetName);
	Data->SetStringField(TEXT("event_name"), EventFName.ToString());
	Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
	Data->SetBoolField(TEXT("created"), bCreated);
	Data->SetBoolField(TEXT("forced_variable"), bForcedVariable);
	Data->SetArrayField(TEXT("output_pins"), ExportOutputPins(Node));

	return FMCPToolResult::Success(
		FString::Printf(TEXT("%s bound event %s.%s on '%s' (node %s)."),
			bCreated ? TEXT("Created") : TEXT("Found existing"),
			*WidgetName, *EventFName.ToString(), *WBP->GetName(), *Node->NodeGuid.ToString()),
		Data);
}

#endif // WITH_EDITOR
