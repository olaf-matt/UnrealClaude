// Copyright Natali Caggiano. All Rights Reserved.

#include "BlueprintEditor.h"
#include "BlueprintGraphEditor.h"
#include "UnrealClaudeModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EditablePinBase.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"

// ===== Variable Management =====

bool FBlueprintEditor::AddVariable(
	UBlueprint* Blueprint,
	const FString& VariableName,
	const FEdGraphPinType& PinType,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	if (!ValidateVariableName(VariableName, OutError))
	{
		return false;
	}

	// Check for existing variable
	FName VarName(*VariableName);
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			OutError = FString::Printf(TEXT("Variable '%s' already exists"), *VariableName);
			return false;
		}
	}

	// Add the variable
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType))
	{
		OutError = TEXT("Failed to add variable");
		return false;
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Added variable '%s' to Blueprint '%s'"),
		*VariableName, *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::RemoveVariable(
	UBlueprint* Blueprint,
	const FString& VariableName,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	FName VarName(*VariableName);

	// Verify variable exists
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found"), *VariableName);
		return false;
	}

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);

	UE_LOG(LogUnrealClaude, Log, TEXT("Removed variable '%s' from Blueprint '%s'"),
		*VariableName, *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::SetVariableInstanceEditable(
	UBlueprint* Blueprint,
	const FString& VariableName,
	bool bInstanceEditable,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	FName VarName(*VariableName);

	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found"), *VariableName);
		return false;
	}

	// SetBlueprintOnlyEditableFlag(false) removes CPF_DisableEditOnInstance → Instance Editable ON
	// SetBlueprintOnlyEditableFlag(true)  adds    CPF_DisableEditOnInstance → Instance Editable OFF
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, !bInstanceEditable);

	UE_LOG(LogUnrealClaude, Log,
		TEXT("Set variable '%s' instance_editable=%s on Blueprint '%s'"),
		*VariableName, bInstanceEditable ? TEXT("true") : TEXT("false"), *Blueprint->GetName());
	return true;
}

// ===== Function Management =====

bool FBlueprintEditor::AddFunction(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	FString& OutError)
{
	return AddFunction(Blueprint, FunctionName, TArray<FBlueprintFunctionParam>(), OutError);
}

bool FBlueprintEditor::AddFunction(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	const TArray<FBlueprintFunctionParam>& InParams,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	if (!ValidateFunctionName(FunctionName, OutError))
	{
		return false;
	}

	// Check for existing function
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			OutError = FString::Printf(TEXT("Function '%s' already exists"), *FunctionName);
			return false;
		}
	}

	// Create function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!NewGraph)
	{
		OutError = TEXT("Failed to create function graph");
		return false;
	}

	// bIsUserCreated=true: user-authored function — allows rename, delete, and signature editing
	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, true, static_cast<UFunction*>(nullptr));

	// Find or create the entry node
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		EntryNode = NewObject<UK2Node_FunctionEntry>(NewGraph);
		EntryNode->CreateNewGuid();
		EntryNode->PostPlacedNewNode();
		EntryNode->AllocateDefaultPins();
		NewGraph->AddNode(EntryNode);
	}

	// Interface function entry nodes must be editable so the Details panel exposes
	// the Inputs/Outputs signature editor. AddFunctionGraph leaves bIsEditable=false
	// for interface blueprints, making the signature permanently frozen.
	if (Blueprint->BlueprintType == BPTYPE_Interface)
	{
		EntryNode->bIsEditable = true;
	}

	// Apply input parameters via UserDefinedPins (same mechanism as the Details panel "+")
	if (InParams.Num() > 0)
	{
		for (const FBlueprintFunctionParam& Param : InParams)
		{
			TSharedPtr<FUserPinInfo> PinInfo = MakeShared<FUserPinInfo>();
			PinInfo->PinName = FName(*Param.Name);
			PinInfo->PinType = Param.PinType;
			PinInfo->DesiredPinDirection = EGPD_Output;
			EntryNode->UserDefinedPins.Add(PinInfo);
		}
		EntryNode->ReconstructNode();
	}

	UE_LOG(LogUnrealClaude, Log, TEXT("Added function '%s' (%d params) to Blueprint '%s'"),
		*FunctionName, InParams.Num(), *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::RemoveFunction(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	// Find function graph
	UEdGraph* GraphToRemove = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			GraphToRemove = Graph;
			break;
		}
	}

	if (!GraphToRemove)
	{
		OutError = FString::Printf(TEXT("Function '%s' not found"), *FunctionName);
		return false;
	}

	FBlueprintEditorUtils::RemoveGraph(Blueprint, GraphToRemove);

	UE_LOG(LogUnrealClaude, Log, TEXT("Removed function '%s' from Blueprint '%s'"),
		*FunctionName, *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::AddFunctionInput(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	const FString& InputName,
	const FEdGraphPinType& PinType,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint is null");
		return false;
	}

	// Find the function graph
	UEdGraph* FuncGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			FuncGraph = Graph;
			break;
		}
	}

	if (!FuncGraph)
	{
		OutError = FString::Printf(TEXT("Function '%s' not found"), *FunctionName);
		return false;
	}

	// Find the entry node
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : FuncGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		OutError = FString::Printf(TEXT("Entry node for function '%s' not found"), *FunctionName);
		return false;
	}

	// Reject duplicate pin names
	for (const TSharedPtr<FUserPinInfo>& Existing : EntryNode->UserDefinedPins)
	{
		if (Existing.IsValid() && Existing->PinName == FName(*InputName))
		{
			OutError = FString::Printf(TEXT("Input '%s' already exists on function '%s'"), *InputName, *FunctionName);
			return false;
		}
	}

	// The entry node must be editable so the function signature accepts new pins.
	// This is required for both Interface and regular Blueprint functions —
	// without it, ReconstructNode ignores UserDefinedPins on non-interface BPs.
	EntryNode->bIsEditable = true;

	TSharedPtr<FUserPinInfo> PinInfo = MakeShared<FUserPinInfo>();
	PinInfo->PinName = FName(*InputName);
	PinInfo->PinType = PinType;
	PinInfo->DesiredPinDirection = EGPD_Output;
	EntryNode->UserDefinedPins.Add(PinInfo);
	EntryNode->ReconstructNode();

	// Structural modification needed so the compiler picks up the new function signature
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogUnrealClaude, Log, TEXT("Added input '%s' to function '%s' on Blueprint '%s'"),
		*InputName, *FunctionName, *Blueprint->GetName());
	return true;
}

// ===== Variable Management Additions =====

bool FBlueprintEditor::SetVariableDefault(
	UBlueprint* Blueprint,
	const FString& VariableName,
	const FString& DefaultValue,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	FName VarName(*VariableName);
	for (FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			Var.DefaultValue = DefaultValue;
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			UE_LOG(LogUnrealClaude, Log, TEXT("Set default for '%s' = '%s' on Blueprint '%s'"),
				*VariableName, *DefaultValue, *Blueprint->GetName());
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *Blueprint->GetName());
	return false;
}

bool FBlueprintEditor::SetVariableExposeOnSpawn(
	UBlueprint* Blueprint,
	const FString& VariableName,
	bool bExposeOnSpawn,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	FName VarName(*VariableName);
	for (FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarName)
		{
			if (bExposeOnSpawn)
				Var.PropertyFlags |= CPF_ExposeOnSpawn;
			else
				Var.PropertyFlags &= ~CPF_ExposeOnSpawn;

			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			UE_LOG(LogUnrealClaude, Log, TEXT("Set expose_on_spawn=%s for '%s' on Blueprint '%s'"),
				bExposeOnSpawn ? TEXT("true") : TEXT("false"), *VariableName, *Blueprint->GetName());
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *Blueprint->GetName());
	return false;
}

bool FBlueprintEditor::RenameVariable(
	UBlueprint* Blueprint,
	const FString& OldName,
	const FString& NewName,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	if (!ValidateVariableName(NewName, OutError)) return false;

	FName OldVarName(*OldName);
	FName NewVarName(*NewName);

	// Verify old name exists
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == OldVarName) { bFound = true; break; }
	}
	if (!bFound)
	{
		OutError = FString::Printf(TEXT("Variable '%s' not found"), *OldName);
		return false;
	}

	// Verify new name is not taken
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == NewVarName)
		{
			OutError = FString::Printf(TEXT("Variable '%s' already exists"), *NewName);
			return false;
		}
	}

	// RenameMemberVariable updates the descriptor AND all graph references
	FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldVarName, NewVarName);

	UE_LOG(LogUnrealClaude, Log, TEXT("Renamed variable '%s' → '%s' on Blueprint '%s'"),
		*OldName, *NewName, *Blueprint->GetName());
	return true;
}

// ===== Component Management =====

bool FBlueprintEditor::AddComponent(
	UBlueprint* Blueprint,
	const FString& ComponentClassName,
	const FString& ComponentName,
	const FString& AssetPath,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	// Resolve component class — try bare name, then with "Component" suffix
	UClass* ComponentClass = FBlueprintGraphEditor::ResolveClassByName(ComponentClassName);
	if (!ComponentClass)
		ComponentClass = FBlueprintGraphEditor::ResolveClassByName(ComponentClassName + TEXT("Component"));
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		OutError = FString::Printf(
			TEXT("Component class '%s' not found or is not an ActorComponent subclass. "
			     "Use the C++ class name without U prefix, e.g. 'NiagaraComponent', 'StaticMeshComponent'."),
			*ComponentClassName);
		return false;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = TEXT("Blueprint has no SimpleConstructionScript — parent class must be Actor-derived");
		return false;
	}

	// Check for duplicate component name
	FName CompName(*ComponentName);
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == CompName)
		{
			OutError = FString::Printf(TEXT("Component '%s' already exists in Blueprint '%s'"),
				*ComponentName, *Blueprint->GetName());
			return false;
		}
	}

	// Create the SCS node
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, CompName);
	if (!NewNode)
	{
		OutError = TEXT("SCS->CreateNode returned null");
		return false;
	}

	// Optionally assign an asset to the component template
	if (!AssetPath.IsEmpty() && NewNode->ComponentTemplate)
	{
		// Build full path: "/Game/path/Asset" → "/Game/path/Asset.Asset"
		FString FullPath = AssetPath;
		if (!FullPath.Contains(TEXT(".")))
			FullPath = FullPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);

		UObject* Asset = LoadObject<UObject>(nullptr, *FullPath);
		if (Asset)
		{
			UActorComponent* Template = NewNode->ComponentTemplate;
			UClass* TemplateClass = Template->GetClass();

			// Try common asset-holding property names in order of likelihood
			static const TArray<FName> AssetPropNames = {
				TEXT("Asset"),             // NiagaraComponent → UNiagaraSystem
				TEXT("StaticMesh"),        // StaticMeshComponent
				TEXT("SkeletalMeshAsset"), // SkeletalMeshComponent (UE5)
				TEXT("SkeletalMesh"),      // SkeletalMeshComponent (UE4 compat)
				TEXT("Sound"),             // AudioComponent
				TEXT("ParticleSystem"),    // ParticleSystemComponent
			};

			bool bAssetSet = false;
			for (FName PropName : AssetPropNames)
			{
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(TemplateClass->FindPropertyByName(PropName)))
				{
					if (Asset->IsA(ObjProp->PropertyClass))
					{
						ObjProp->SetObjectPropertyValue_InContainer(Template, Asset);
						bAssetSet = true;
						UE_LOG(LogUnrealClaude, Log, TEXT("AddComponent: set %s.%s = '%s'"),
							*ComponentName, *PropName.ToString(), *AssetPath);
						break;
					}
				}
			}

			if (!bAssetSet)
			{
				UE_LOG(LogUnrealClaude, Warning,
					TEXT("AddComponent: asset '%s' loaded but no matching property found on %s — component added without asset"),
					*AssetPath, *TemplateClass->GetName());
			}
		}
		else
		{
			UE_LOG(LogUnrealClaude, Warning,
				TEXT("AddComponent: asset '%s' not found — component '%s' added without asset"),
				*AssetPath, *ComponentName);
		}
	}

	// Attach to scene hierarchy: SceneComponents go under DefaultSceneRoot;
	// non-scene ActorComponents are added directly to the SCS.
	if (ComponentClass->IsChildOf(USceneComponent::StaticClass()))
	{
		USCS_Node* Root = SCS->GetDefaultSceneRootNode();
		if (Root)
			Root->AddChildNode(NewNode);
		else
			SCS->AddNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogUnrealClaude, Log, TEXT("Added component '%s' (%s) to Blueprint '%s'"),
		*ComponentName, *ComponentClassName, *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::RemoveComponent(
	UBlueprint* Blueprint,
	const FString& ComponentName,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = TEXT("Blueprint has no SimpleConstructionScript");
		return false;
	}

	FName CompName(*ComponentName);
	USCS_Node* NodeToRemove = nullptr;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == CompName)
		{
			NodeToRemove = Node;
			break;
		}
	}

	if (!NodeToRemove)
	{
		OutError = FString::Printf(TEXT("Component '%s' not found in Blueprint '%s'"),
			*ComponentName, *Blueprint->GetName());
		return false;
	}

	SCS->RemoveNode(NodeToRemove);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogUnrealClaude, Log, TEXT("Removed component '%s' from Blueprint '%s'"),
		*ComponentName, *Blueprint->GetName());
	return true;
}

bool FBlueprintEditor::SetComponentProperty(
	UBlueprint* Blueprint,
	const FString& ComponentName,
	const FString& PropertyName,
	const FString& Value,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		OutError = TEXT("Blueprint has no SimpleConstructionScript");
		return false;
	}

	FName CompName(*ComponentName);
	USCS_Node* TargetNode = nullptr;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == CompName)
		{
			TargetNode = Node;
			break;
		}
	}

	if (!TargetNode || !TargetNode->ComponentTemplate)
	{
		OutError = FString::Printf(TEXT("Component '%s' not found in Blueprint '%s'"),
			*ComponentName, *Blueprint->GetName());
		return false;
	}

	UActorComponent* Template = TargetNode->ComponentTemplate;
	UClass* TemplateClass = Template->GetClass();

	FProperty* Prop = TemplateClass->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		// Collect first 20 property names for a useful error message
		TArray<FString> PropNames;
		for (TFieldIterator<FProperty> It(TemplateClass); It && PropNames.Num() < 20; ++It)
			PropNames.Add(It->GetName());
		OutError = FString::Printf(
			TEXT("Property '%s' not found on component '%s' (%s). Sample properties: %s"),
			*PropertyName, *ComponentName, *TemplateClass->GetName(),
			*FString::Join(PropNames, TEXT(", ")));
		return false;
	}

	// Object reference properties: load the asset and assign
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		FString FullPath = Value;
		if (!FullPath.Contains(TEXT(".")))
			FullPath = FullPath + TEXT(".") + FPaths::GetBaseFilename(Value);

		UObject* Asset = LoadObject<UObject>(nullptr, *FullPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Could not load asset '%s' for property '%s'"), *Value, *PropertyName);
			return false;
		}
		if (!Asset->IsA(ObjProp->PropertyClass))
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a %s (required by property '%s')"),
				*Value, *ObjProp->PropertyClass->GetName(), *PropertyName);
			return false;
		}
		ObjProp->SetObjectPropertyValue_InContainer(Template, Asset);
	}
	else
	{
		// Primitive / struct properties: import from string
		if (!Prop->ImportText_InContainer(*Value, Template, Template, PPF_None))
		{
			OutError = FString::Printf(TEXT("Failed to set property '%s' = '%s' on component '%s'"),
				*PropertyName, *Value, *ComponentName);
			return false;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	UE_LOG(LogUnrealClaude, Log, TEXT("Set component property '%s.%s' = '%s' on Blueprint '%s'"),
		*ComponentName, *PropertyName, *Value, *Blueprint->GetName());
	return true;
}

// ===== Blueprint Class Management =====

bool FBlueprintEditor::AddInterface(
	UBlueprint* Blueprint,
	const FString& InterfaceName,
	FString& OutError)
{
	if (!Blueprint) { OutError = TEXT("Blueprint is null"); return false; }

	// Try to resolve the interface class — Blueprint interfaces have a generated _C class
	UClass* InterfaceClass = FBlueprintGraphEditor::ResolveClassByName(InterfaceName + TEXT("_C"));
	if (!InterfaceClass)
		InterfaceClass = FBlueprintGraphEditor::ResolveClassByName(InterfaceName);

	if (!InterfaceClass)
	{
		OutError = FString::Printf(TEXT("Interface class '%s' not found. "
			"Make sure it is a Blueprint Interface asset and the name is correct."), *InterfaceName);
		return false;
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		OutError = FString::Printf(TEXT("'%s' is not an interface class"), *InterfaceName);
		return false;
	}

	// Prevent duplicates
	for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
	{
		if (Desc.Interface == InterfaceClass)
		{
			OutError = FString::Printf(TEXT("Blueprint '%s' already implements '%s'"),
				*Blueprint->GetName(), *InterfaceName);
			return false;
		}
	}

	// ImplementNewInterface creates stub function graphs for all interface functions
	FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetFName());

	UE_LOG(LogUnrealClaude, Log, TEXT("Added interface '%s' to Blueprint '%s'"),
		*InterfaceName, *Blueprint->GetName());
	return true;
}

// ===== Name Validation =====

bool FBlueprintEditor::ValidateVariableName(const FString& VariableName, FString& OutError)
{
	if (VariableName.IsEmpty())
	{
		OutError = TEXT("Variable name cannot be empty");
		return false;
	}

	if (VariableName.Len() > MaxNameLength)
	{
		OutError = FString::Printf(TEXT("Variable name exceeds maximum length of %d characters"), MaxNameLength);
		return false;
	}

	// Must start with letter or underscore
	if (!FChar::IsAlpha(VariableName[0]) && VariableName[0] != TEXT('_'))
	{
		OutError = TEXT("Variable name must start with a letter or underscore");
		return false;
	}

	// Only alphanumeric and underscore
	for (TCHAR C : VariableName)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_'))
		{
			OutError = FString::Printf(TEXT("Variable name contains invalid character: '%c'"), C);
			return false;
		}
	}

	return true;
}

bool FBlueprintEditor::ValidateFunctionName(const FString& FunctionName, FString& OutError)
{
	// Same validation rules as variables
	if (FunctionName.IsEmpty())
	{
		OutError = TEXT("Function name cannot be empty");
		return false;
	}

	if (FunctionName.Len() > MaxNameLength)
	{
		OutError = FString::Printf(TEXT("Function name exceeds maximum length of %d characters"), MaxNameLength);
		return false;
	}

	if (!FChar::IsAlpha(FunctionName[0]) && FunctionName[0] != TEXT('_'))
	{
		OutError = TEXT("Function name must start with a letter or underscore");
		return false;
	}

	for (TCHAR C : FunctionName)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_'))
		{
			OutError = FString::Printf(TEXT("Function name contains invalid character: '%c'"), C);
			return false;
		}
	}

	return true;
}

// ===== Type Conversion =====

bool FBlueprintEditor::ParsePinType(
	const FString& TypeString,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	OutPinType.ResetToDefaults();
	FString CleanType = TypeString.TrimStartAndEnd();

	// Handle container types
	if (ParseContainerType(CleanType, OutPinType, OutError))
	{
		return OutError.IsEmpty();
	}

	// Primitive types
	if (CleanType == TEXT("bool") || CleanType == TEXT("Boolean"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (CleanType == TEXT("int") || CleanType == TEXT("int32") || CleanType == TEXT("Integer"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (CleanType == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	if (CleanType == TEXT("float") || CleanType == TEXT("Float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (CleanType == TEXT("double") || CleanType == TEXT("Double"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (CleanType == TEXT("byte") || CleanType == TEXT("uint8") || CleanType == TEXT("Byte"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		return true;
	}
	if (CleanType == TEXT("FString") || CleanType == TEXT("String"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (CleanType == TEXT("FName") || CleanType == TEXT("Name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (CleanType == TEXT("FText") || CleanType == TEXT("Text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}

	// Struct and object types
	if (ParseStructType(CleanType, OutPinType, OutError))
	{
		return true;
	}

	// Object references — explicit pointer suffix (e.g. "MaterialInstanceDynamic*")
	if (CleanType.EndsWith(TEXT("*")))
	{
		FString ClassName = CleanType.LeftChop(1).TrimEnd();
		UClass* Class = FBlueprintGraphEditor::ResolveClassByName(ClassName);
		if (Class)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = Class;
			return true;
		}
		OutError = FString::Printf(
			TEXT("Unknown class '%s'. Use the C++ name without U prefix (e.g. 'ExponentialHeightFogComponent' not 'UExponentialHeightFogComponent')."),
			*ClassName);
		return false;
	}

	// Last-resort: try resolving as an object-reference class without the * suffix.
	// Handles "MaterialInstanceDynamic", "NiagaraComponent", "SkeletalMeshComponent", etc.
	{
		UClass* Class = FBlueprintGraphEditor::ResolveClassByName(CleanType);
		if (Class)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = Class;
			return true;
		}
	}

	OutError = FString::Printf(TEXT("Unknown type: '%s'. Supported: bool, int, float, double, byte, FString, FName, FText, Vector, Rotator, Transform, LinearColor, TArray<T>, TSet<T>, T[] (shorthand array), or any C++ class name (with or without * suffix)."), *TypeString);
	return false;
}

bool FBlueprintEditor::ParseContainerType(
	const FString& TypeString,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	// TArray<T>
	if (TypeString.StartsWith(TEXT("TArray<")) && TypeString.EndsWith(TEXT(">")))
	{
		FString InnerType = TypeString.Mid(7, TypeString.Len() - 8);
		FEdGraphPinType InnerPinType;
		if (!ParsePinType(InnerType, InnerPinType, OutError))
		{
			return true; // Error set
		}
		OutPinType = InnerPinType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}

	// TSet<T>
	if (TypeString.StartsWith(TEXT("TSet<")) && TypeString.EndsWith(TEXT(">")))
	{
		FString InnerType = TypeString.Mid(5, TypeString.Len() - 6);
		FEdGraphPinType InnerPinType;
		if (!ParsePinType(InnerType, InnerPinType, OutError))
		{
			return true; // Error set
		}
		OutPinType = InnerPinType;
		OutPinType.ContainerType = EPinContainerType::Set;
		return true;
	}

	// TODO-44: Shorthand array syntax: "T[]"  (e.g. "Vector[]", "float[]", "int[]", "Actor[]")
	// Expand to TArray<T> and recurse.
	if (TypeString.EndsWith(TEXT("[]")))
	{
		FString InnerType = TypeString.LeftChop(2);
		FEdGraphPinType InnerPinType;
		if (!ParsePinType(InnerType, InnerPinType, OutError))
		{
			return true; // Error already set
		}
		OutPinType = InnerPinType;
		OutPinType.ContainerType = EPinContainerType::Array;
		return true;
	}

	return false; // Not a container type
}

bool FBlueprintEditor::ParseStructType(
	const FString& TypeName,
	FEdGraphPinType& OutPinType,
	FString& OutError)
{
	// Common struct types with TBaseStructure
	if (TypeName == TEXT("FVector") || TypeName == TEXT("Vector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return true;
	}
	if (TypeName == TEXT("FRotator") || TypeName == TEXT("Rotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		return true;
	}
	if (TypeName == TEXT("FTransform") || TypeName == TEXT("Transform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
		return true;
	}
	if (TypeName == TEXT("FLinearColor") || TypeName == TEXT("LinearColor"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		return true;
	}
	if (TypeName == TEXT("FColor") || TypeName == TEXT("Color"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
		return true;
	}
	if (TypeName == TEXT("FVector2D") || TypeName == TEXT("Vector2D"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
		return true;
	}

	// Try finding by name
	UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *TypeName);
	if (Struct)
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = Struct;
		return true;
	}

	return false;
}

FString FBlueprintEditor::PinTypeToString(const FEdGraphPinType& PinType)
{
	// Container prefix/suffix
	FString Prefix, Suffix;
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Prefix = TEXT("TArray<");
		Suffix = TEXT(">");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Prefix = TEXT("TSet<");
		Suffix = TEXT(">");
	}

	// Base type name
	FString TypeName;

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		TypeName = TEXT("bool");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		TypeName = TEXT("int32");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		TypeName = TEXT("int64");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		TypeName = (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
			? TEXT("double") : TEXT("float");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		// FByteProperty with a non-null Enum pointer means a UENUM variable; prefer enum name over "byte"
		if (UEnum* Enum = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			TypeName = Enum->GetName();
		}
		else
		{
			TypeName = TEXT("byte");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		TypeName = TEXT("FString");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		TypeName = TEXT("FName");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		TypeName = TEXT("FText");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
		{
			TypeName = Struct->GetName();
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
	         PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
	{
		if (UClass* Class = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			TypeName = Class->GetName() + TEXT("*");
		}
	}
	else
	{
		TypeName = PinType.PinCategory.ToString();
	}

	return Prefix + TypeName + Suffix;
}
