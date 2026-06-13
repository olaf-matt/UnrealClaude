// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * MCP Tool for material operations.
 *
 * Provides operations for:
 * - Creating Material Instances (Constant or Dynamic)
 * - Setting material parameters (scalar, vector, texture)
 * - Assigning materials to Skeletal Mesh asset slots
 *
 * Operations:
 * - create_material_instance: Create a new UMaterialInstanceConstant asset
 * - set_material_parameters: Set parameters on an existing material instance
 * - set_skeletal_mesh_material: Set a material slot on a USkeletalMesh asset
 * - set_actor_material: Assign a material to an actor's mesh component at runtime
 * - get_material_info: Get information about a material or material instance
 */
class FMCPTool_Material : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	// Operation handlers
	FMCPToolResult ExecuteCreateMaterial(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteCreateMaterialInstance(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetMaterialParameters(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetSkeletalMeshMaterial(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteSetActorMaterial(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteGetMaterialInfo(const TSharedRef<FJsonObject>& Params);
	// Phase 2b: edit a constant expression node (Constant / Constant2/3/4Vector) addressed by node_id (GUID)
	FMCPToolResult ExecuteSetExpressionValue(const TSharedRef<FJsonObject>& Params);
	// Phase 2b: regenerate colliding MaterialExpressionGuids so every node_id is unique
	FMCPToolResult ExecuteRepairExpressionIds(const TSharedRef<FJsonObject>& Params);
	// Phase 2c: material-graph topology editing (UMaterial AND UMaterialFunction)
	FMCPToolResult ExecuteAddExpression(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteConnectExpression(const TSharedRef<FJsonObject>& Params);
	FMCPToolResult ExecuteDeleteExpression(const TSharedRef<FJsonObject>& Params);

	// Helper methods
	bool SetScalarParameter(class UMaterialInstanceConstant* MatInst, const FString& ParamName, float Value, FString& OutError);
	bool SetVectorParameter(class UMaterialInstanceConstant* MatInst, const FString& ParamName, const FLinearColor& Value, FString& OutError);
	bool SetTextureParameter(class UMaterialInstanceConstant* MatInst, const FString& ParamName, const FString& TexturePath, FString& OutError);
	bool ApplyParametersFromJson(class UMaterialInstanceConstant* MatInst, const TSharedPtr<FJsonObject>& ParamsObj, FString& OutError);

	// Phase 2: set parameter DEFAULTS on a base UMaterial (mutates parameter expression nodes)
	bool ApplyParametersToBaseMaterial(class UMaterial* Material, const TSharedPtr<FJsonObject>& ParamsObj, FString& OutError);

	// Utility
	TSharedPtr<FJsonObject> BuildMaterialInfoJson(class UMaterialInterface* Material);
	TArray<TSharedPtr<FJsonValue>> GetMaterialParameters(class UMaterialInterface* Material);
};
