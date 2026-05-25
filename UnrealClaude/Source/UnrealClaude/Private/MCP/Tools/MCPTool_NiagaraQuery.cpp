// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_NiagaraQuery.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"

#include "NiagaraSystem.h"
#include "NiagaraParameterStore.h"
#include "NiagaraTypes.h"

FMCPToolInfo FMCPTool_NiagaraQuery::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("niagara_query");
	Info.Description = TEXT(
		"Query a NiagaraSystem asset's exposed user parameters (read-only).\n\n"
		"Returns the names, types, and data-interface flag for every parameter in the\n"
		"system's exposed parameter store.  Use this before wiring a readback system\n"
		"to confirm it has the expected inputs (e.g. Pontoons, Blueprint, bExportBuoyancy).\n\n"
		"Operations:\n"
		"  inspect  — list all exposed user parameters\n\n"
		"Example:\n"
		"  { \"operation\": \"inspect\", \"system_path\": \"/Game/03/Particles/FX_Syst_Readback_03\" }"
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("Operation to perform. Currently: inspect"), true));
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

	if (Operation == TEXT("inspect"))
	{
		return ExecuteInspect(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: '%s'. Valid operations: inspect"), *Operation));
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
