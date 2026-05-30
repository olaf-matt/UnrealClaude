// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_SetNiagaraVariable.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"

#include "NiagaraComponent.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

// ============================================================
//  GetInfo
// ============================================================

FMCPToolInfo FMCPTool_SetNiagaraVariable::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("set_niagara_variable");
	Info.Description = TEXT(
		"Set a user parameter override on a NiagaraComponent attached to a level actor.\n\n"
		"Equivalent to calling SetNiagaraVariableFloat/Vec3/Bool/Color/Int from Blueprint.\n"
		"Useful for live-tuning Niagara parameters during development without Blueprint graph changes.\n\n"
		"Type auto-detection (when 'type' is omitted):\n"
		"  JSON number              → float\n"
		"  JSON bool                → bool\n"
		"  JSON {X, Y, Z}          → vec3 (FVector)\n"
		"  JSON {R, G, B} or {R,G,B,A} → color (FLinearColor)\n\n"
		"Examples:\n"
		"  Float:  { \"actor_name\": \"BP_ShallowWater_0\", \"variable_name\": \"User.WaterDepth\", \"value\": 25.0 }\n"
		"  Int:    { \"actor_name\": \"FX_MySystem_0\", \"variable_name\": \"User.Seed\", \"value\": 42, \"type\": \"int\" }\n"
		"  Vec3:   { \"actor_name\": \"FX_MySystem_0\", \"variable_name\": \"User.GravityDir\",\n"
		"           \"value\": {\"X\": 0.1, \"Y\": 0.0, \"Z\": -0.99} }\n"
		"  Color:  { \"actor_name\": \"FX_MySystem_0\", \"variable_name\": \"User.TintColor\",\n"
		"           \"value\": {\"R\": 0.0, \"G\": 0.5, \"B\": 1.0, \"A\": 1.0} }\n"
		"  Bool:   { \"actor_name\": \"FX_MySystem_0\", \"variable_name\": \"User.bEnabled\", \"value\": true }\n\n"
		"The actor must have a NiagaraComponent (either be a NiagaraActor or a BP with one).\n"
		"The variable must exist in the NiagaraSystem's exposed user parameters."
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("actor_name"), TEXT("string"),
		TEXT("Outliner label of the actor with a NiagaraComponent"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("variable_name"), TEXT("string"),
		TEXT("Full user parameter name, e.g. \"User.WaterDepth\" or \"WindControl.WindSpeed\""), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("value"), TEXT("any"),
		TEXT("Value to set: number (float/int), bool, {X,Y,Z} for vec3, or {R,G,B,A} for color"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("type"), TEXT("string"),
		TEXT("Optional type override: \"float\" | \"int\" | \"bool\" | \"vec3\" | \"color\". "
			 "Auto-detected from value shape if omitted."), false));

	Info.Annotations = FMCPToolAnnotations::Modifying();
	return Info;
}

// ============================================================
//  FindNiagaraComponent
// ============================================================

UNiagaraComponent* FMCPTool_SetNiagaraVariable::FindNiagaraComponent(AActor* Actor) const
{
	if (!Actor) return nullptr;

	// Prefer a component named "NiagaraComponent" (NiagaraActor's default component)
	TArray<UNiagaraComponent*> NiagaraComponents;
	Actor->GetComponents<UNiagaraComponent>(NiagaraComponents);

	return NiagaraComponents.Num() > 0 ? NiagaraComponents[0] : nullptr;
}

// ============================================================
//  Execute
// ============================================================

FMCPToolResult FMCPTool_SetNiagaraVariable::Execute(const TSharedRef<FJsonObject>& Params)
{
	// --- Required parameters ---
	FString ActorName, VarName;
	TOptional<FMCPToolResult> Error;

	if (!ExtractActorName(Params, TEXT("actor_name"), ActorName, Error))      return Error.GetValue();
	if (!ExtractRequiredString(Params, TEXT("variable_name"), VarName, Error)) return Error.GetValue();

	// --- Value field (required, but any JSON type) ---
	const TSharedPtr<FJsonValue>* ValueFieldPtr = Params->Values.Find(TEXT("value"));
	if (!ValueFieldPtr || !ValueFieldPtr->IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: value"));
	}
	const TSharedPtr<FJsonValue>& JsonVal = *ValueFieldPtr;

	// Optional explicit type override
	FString TypeHint = ExtractOptionalString(Params, TEXT("type"), TEXT("")).ToLower();

	// --- Find actor and NiagaraComponent ---
	UWorld* World;
	if (auto CtxError = ValidateEditorContext(World)) return CtxError.GetValue();

	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Actor not found: '%s'. Use get_level_actors to find the correct name."), *ActorName));
	}

	UNiagaraComponent* NiagaraComp = FindNiagaraComponent(Actor);
	if (!NiagaraComp)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Actor '%s' has no NiagaraComponent. Make sure it's a NiagaraActor or a "
				 "Blueprint with a NiagaraComponent."), *ActorName));
	}

	// --- Normalise: if value arrived as a string (happens when schema type is "any"),
	//     try to re-parse it as the actual intended JSON type. ---
	TSharedPtr<FJsonValue> ReparsedVal;
	if (JsonVal->Type == EJson::String)
	{
		FString Str = JsonVal->AsString().TrimStartAndEnd();
		if (Str.StartsWith(TEXT("{")) || Str.StartsWith(TEXT("[")))
		{
			TSharedPtr<FJsonObject> ParsedObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
			if (FJsonSerializer::Deserialize(Reader, ParsedObj) && ParsedObj.IsValid())
			{
				ReparsedVal = MakeShared<FJsonValueObject>(ParsedObj);
			}
		}
		else if (Str.ToLower() == TEXT("true"))  { ReparsedVal = MakeShared<FJsonValueBoolean>(true);  }
		else if (Str.ToLower() == TEXT("false")) { ReparsedVal = MakeShared<FJsonValueBoolean>(false); }
		else
		{
			// Try numeric — FCString::Atod returns 0.0 for non-numeric; guard with IsNumeric check.
			if (!Str.IsEmpty() && (FChar::IsDigit(Str[0]) || Str[0] == TEXT('-') || Str[0] == TEXT('.')))
			{
				ReparsedVal = MakeShared<FJsonValueNumber>(FCString::Atod(*Str));
			}
		}
	}
	const TSharedPtr<FJsonValue>& ActualVal = ReparsedVal.IsValid() ? ReparsedVal : JsonVal;

	// --- Detect type and call the appropriate setter ---

	EJson ValType = ActualVal->Type;
	FString TypeApplied;

	// --- bool ---
	if (ValType == EJson::Boolean || TypeHint == TEXT("bool"))
	{
		bool bVal = false;
		if (ValType == EJson::Boolean)
		{
			bVal = ActualVal->AsBool();
		}
		else
		{
			// TypeHint forced "bool" but value is a number — treat 0 as false
			double NumVal;
			if (ActualVal->TryGetNumber(NumVal)) bVal = (NumVal != 0.0);
		}
		NiagaraComp->SetNiagaraVariableBool(VarName, bVal);
		TypeApplied = FString::Printf(TEXT("bool = %s"), bVal ? TEXT("true") : TEXT("false"));
	}
	// --- object (vec3 or color) ---
	else if (ValType == EJson::Object || TypeHint == TEXT("vec3") || TypeHint == TEXT("color"))
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!ActualVal->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Expected an object value ({X,Y,Z} or {R,G,B,A}) for variable '%s', "
					 "but got type %d."), *VarName, (int32)ValType));
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

		// Detect vec3 vs color from key names, or from TypeHint
		bool bHasXYZ = Obj->HasField(TEXT("X")) || Obj->HasField(TEXT("x"))
		             || Obj->HasField(TEXT("Y")) || Obj->HasField(TEXT("y"))
		             || Obj->HasField(TEXT("Z")) || Obj->HasField(TEXT("z"));
		bool bHasRGB = Obj->HasField(TEXT("R")) || Obj->HasField(TEXT("r"))
		             || Obj->HasField(TEXT("G")) || Obj->HasField(TEXT("g"))
		             || Obj->HasField(TEXT("B")) || Obj->HasField(TEXT("b"));

		bool bIsColor = (TypeHint == TEXT("color")) || (bHasRGB && !bHasXYZ);
		bool bIsVec3  = (TypeHint == TEXT("vec3"))  || (bHasXYZ && !bIsColor);

		if (bIsColor)
		{
			double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
			Obj->TryGetNumberField(TEXT("R"), R); Obj->TryGetNumberField(TEXT("r"), R);
			Obj->TryGetNumberField(TEXT("G"), G); Obj->TryGetNumberField(TEXT("g"), G);
			Obj->TryGetNumberField(TEXT("B"), B); Obj->TryGetNumberField(TEXT("b"), B);
			Obj->TryGetNumberField(TEXT("A"), A); Obj->TryGetNumberField(TEXT("a"), A);
			NiagaraComp->SetNiagaraVariableLinearColor(VarName, FLinearColor((float)R, (float)G, (float)B, (float)A));
			TypeApplied = FString::Printf(TEXT("color = (R=%.3f, G=%.3f, B=%.3f, A=%.3f)"), R, G, B, A);
		}
		else if (bIsVec3)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			Obj->TryGetNumberField(TEXT("X"), X); Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("Y"), Y); Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("Z"), Z); Obj->TryGetNumberField(TEXT("z"), Z);
			NiagaraComp->SetNiagaraVariableVec3(VarName, FVector(X, Y, Z));
			TypeApplied = FString::Printf(TEXT("vec3 = (%.3f, %.3f, %.3f)"), X, Y, Z);
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Cannot determine vector type for variable '%s'. Use X/Y/Z keys for vec3 "
					 "or R/G/B/A keys for color, or pass explicit 'type' parameter."), *VarName));
		}
	}
	// --- int ---
	else if (ValType == EJson::Number && TypeHint == TEXT("int"))
	{
		double NumVal;
		ActualVal->TryGetNumber(NumVal);
		NiagaraComp->SetNiagaraVariableInt(VarName, (int32)NumVal);
		TypeApplied = FString::Printf(TEXT("int = %d"), (int32)NumVal);
	}
	// --- float (default for JSON number) ---
	else if (ValType == EJson::Number || TypeHint == TEXT("float"))
	{
		double NumVal = 0.0;
		ActualVal->TryGetNumber(NumVal);
		NiagaraComp->SetNiagaraVariableFloat(VarName, (float)NumVal);
		TypeApplied = FString::Printf(TEXT("float = %.6f"), (float)NumVal);
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unsupported value type for variable '%s'. "
				 "Provide a number (float/int), bool, {X,Y,Z} object (vec3), "
				 "or {R,G,B,A} object (color)."), *VarName));
	}

	// --- Return result ---
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), ActorName);
	Result->SetStringField(TEXT("component_name"), NiagaraComp->GetName());
	Result->SetStringField(TEXT("variable_name"), VarName);
	Result->SetStringField(TEXT("value_applied"), TypeApplied);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set Niagara variable '%s' on '%s': %s"),
			*VarName, *ActorName, *TypeApplied), Result);
}
