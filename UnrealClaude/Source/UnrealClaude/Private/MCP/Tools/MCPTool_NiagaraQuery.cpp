// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_NiagaraQuery.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"

#include "NiagaraSystem.h"
#include "NiagaraParameterStore.h"
#include "NiagaraTypes.h"
#include "NiagaraParameterCollection.h"

FMCPToolInfo FMCPTool_NiagaraQuery::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("niagara_query");
	Info.Description = TEXT(
		"Query NiagaraSystem user parameters or NiagaraParameterCollection parameters (read-only).\n\n"
		"Operations:\n"
		"  inspect            — list all exposed user parameters on a NiagaraSystem\n"
		"  inspect_collection — list parameters and default values from a NiagaraParameterCollection\n\n"
		"Example (inspect system):\n"
		"  { \"operation\": \"inspect\", \"system_path\": \"/Game/03/Particles/FX_Syst_Readback_03\" }\n\n"
		"Example (inspect collection):\n"
		"  { \"operation\": \"inspect_collection\", \"system_path\": \"/Game/Particles/Collections/FX_Col_NiagaraBuoyancy\" }"
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("Operation to perform: inspect | inspect_collection"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("system_path"), TEXT("string"),
		TEXT("Asset path of the NiagaraSystem to inspect, e.g. /Game/03/Particles/FX_Syst_Readback_03"), true));

	Info.Annotations = FMCPToolAnnotations::ReadOnly();
	return Info;
}

FMCPToolResult FMCPTool_NiagaraQuery::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("inspect"))           return ExecuteInspect(Params);
	if (Operation == TEXT("inspect_collection")) return ExecuteInspectCollection(Params);

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid operations: inspect, inspect_collection"), *Operation));
}

FMCPToolResult FMCPTool_NiagaraQuery::ExecuteInspect(const TSharedRef<FJsonObject>& Params)
{
	FString SystemPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), SystemPath, Error))
	{
		return Error.GetValue();
	}

	if (!ValidateBlueprintPathParam(SystemPath, Error))
	{
		return Error.GetValue();
	}

	// Load the NiagaraSystem asset
	UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!NiagaraSystem)
	{
		// Retry without trailing asset-name duplication (some paths omit the second segment)
		NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *(SystemPath + TEXT(".") + FPackageName::GetShortName(SystemPath)));
	}
	if (!NiagaraSystem)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraSystem at path: %s. "
				 "Make sure the path points to a NiagaraSystem asset and the editor has loaded the content package."),
			*SystemPath));
	}

	// Enumerate exposed user parameters
	const FNiagaraUserRedirectionParameterStore& ExposedParams = NiagaraSystem->GetExposedParameters();

	TArray<FNiagaraVariable> Variables;
	ExposedParams.GetParameters(Variables);

	TArray<TSharedPtr<FJsonValue>> ParamsArray;
	for (const FNiagaraVariable& Var : Variables)
	{
		const FNiagaraTypeDefinition& TypeDef = Var.GetType();
		if (!TypeDef.IsValid())
		{
			continue;
		}

		// Build a human-readable type string
		FString TypeStr;
		if (const UClass* VarClass = TypeDef.GetClass())
		{
			TypeStr = FString::Printf(TEXT("object:%s"), *VarClass->GetName());
		}
		else if (const UScriptStruct* VarStruct = Cast<UScriptStruct>(TypeDef.GetStruct()))
		{
			TypeStr = FString::Printf(TEXT("struct:%s"), *VarStruct->GetName());
		}
		else
		{
			TypeStr = TypeDef.GetName();
		}

		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.GetName().ToString());
		VarObj->SetStringField(TEXT("type"), TypeStr);
		VarObj->SetBoolField(TEXT("is_data_interface"), TypeDef.IsDataInterface());

		ParamsArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("system_path"), SystemPath);
	ResultData->SetStringField(TEXT("system_name"), NiagaraSystem->GetName());
	ResultData->SetNumberField(TEXT("parameter_count"), ParamsArray.Num());
	ResultData->SetArrayField(TEXT("parameters"), ParamsArray);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("NiagaraSystem '%s' has %d exposed parameter(s)"),
			*NiagaraSystem->GetName(), ParamsArray.Num()),
		ResultData);
}

// ============================================================
//  inspect_collection (TODO-26)
//  Read parameters and default values from a NiagaraParameterCollection.
// ============================================================

FMCPToolResult FMCPTool_NiagaraQuery::ExecuteInspectCollection(const TSharedRef<FJsonObject>& Params)
{
	FString CollectionPath;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("system_path"), CollectionPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(CollectionPath, Error))
	{
		return Error.GetValue();
	}

	UNiagaraParameterCollection* NPC = LoadObject<UNiagaraParameterCollection>(nullptr, *CollectionPath);
	if (!NPC)
	{
		const FString FullPath = CollectionPath + TEXT(".") + FPackageName::GetShortName(CollectionPath);
		NPC = LoadObject<UNiagaraParameterCollection>(nullptr, *FullPath);
	}
	if (!NPC)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load NiagaraParameterCollection at '%s'. "
				 "Ensure the path points to a .uasset of type NiagaraParameterCollection."),
			*CollectionPath));
	}

	const TArray<FNiagaraVariable>& Parameters = NPC->GetParameters();

	// Default values live in the default instance's parameter store
	UNiagaraParameterCollectionInstance* DefaultInstance = NPC->GetDefaultInstance();
	const FNiagaraParameterStore* DefaultStore = DefaultInstance
		? &DefaultInstance->GetParameterStore() : nullptr;

	TArray<TSharedPtr<FJsonValue>> ParamsArray;
	for (const FNiagaraVariable& Var : Parameters)
	{
		const FNiagaraTypeDefinition& TypeDef = Var.GetType();
		if (!TypeDef.IsValid()) continue;

		FString TypeStr;
		if (const UClass* VarClass = TypeDef.GetClass())
			TypeStr = FString::Printf(TEXT("object:%s"), *VarClass->GetName());
		else if (const UScriptStruct* VarStruct = Cast<UScriptStruct>(TypeDef.GetStruct()))
			TypeStr = FString::Printf(TEXT("struct:%s"), *VarStruct->GetName());
		else
			TypeStr = TypeDef.GetName();

		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.GetName().ToString());
		VarObj->SetStringField(TEXT("type"), TypeStr);
		VarObj->SetBoolField  (TEXT("is_data_interface"), TypeDef.IsDataInterface());

		// Try to read the default value for known scalar/vector types
		if (DefaultStore && !TypeDef.IsDataInterface())
		{
			const uint8* RawData = DefaultStore->GetParameterData(Var);
			if (RawData)
			{
				if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
				{
					VarObj->SetNumberField(TEXT("default_value"), (double)(*(const float*)RawData));
				}
				else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
				{
					VarObj->SetNumberField(TEXT("default_value"), (double)(*(const int32*)RawData));
				}
				else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
				{
					VarObj->SetBoolField(TEXT("default_value"), (*(const int32*)RawData) != 0);
				}
				else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
				{
					const FVector3f* V = (const FVector3f*)RawData;
					TSharedPtr<FJsonObject> VJ = MakeShared<FJsonObject>();
					VJ->SetNumberField(TEXT("X"), V->X);
					VJ->SetNumberField(TEXT("Y"), V->Y);
					VJ->SetNumberField(TEXT("Z"), V->Z);
					VarObj->SetObjectField(TEXT("default_value"), VJ);
				}
				else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
				{
					const FVector2f* V = (const FVector2f*)RawData;
					TSharedPtr<FJsonObject> VJ = MakeShared<FJsonObject>();
					VJ->SetNumberField(TEXT("X"), V->X);
					VJ->SetNumberField(TEXT("Y"), V->Y);
					VarObj->SetObjectField(TEXT("default_value"), VJ);
				}
				else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
				{
					const FLinearColor* C = (const FLinearColor*)RawData;
					TSharedPtr<FJsonObject> CJ = MakeShared<FJsonObject>();
					CJ->SetNumberField(TEXT("R"), C->R);
					CJ->SetNumberField(TEXT("G"), C->G);
					CJ->SetNumberField(TEXT("B"), C->B);
					CJ->SetNumberField(TEXT("A"), C->A);
					VarObj->SetObjectField(TEXT("default_value"), CJ);
				}
			}
		}

		ParamsArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("collection_path"), CollectionPath);
	ResultData->SetStringField(TEXT("collection_name"), NPC->GetName());
	ResultData->SetNumberField(TEXT("parameter_count"), ParamsArray.Num());
	ResultData->SetArrayField (TEXT("parameters"),      ParamsArray);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("NiagaraParameterCollection '%s' has %d parameter(s)"),
			*NPC->GetName(), ParamsArray.Num()),
		ResultData);
}
