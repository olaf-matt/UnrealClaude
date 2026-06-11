// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_Material.h"
#include "MCP/MCPParamValidator.h"
#include "UnrealClaudeModule.h"
#include "Editor.h"

#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
// UMaterialEditorOnlyData is declared in Materials/Material.h (already included)
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonValue.h"

FMCPToolInfo FMCPTool_Material::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("material");
	Info.Description = TEXT(
		"Material instance creation, parameter editing, and assignment for actors and meshes.\n\n"
		"OPERATION → REQUIRED PARAMS:\n"
		"  create_material           → asset_name [, package_path, blend_mode, base_color, roughness, metallic, opacity, specular]\n"
		"  create_material_instance  → asset_name, parent_material [, package_path, parameters]\n"
		"  set_material_parameters   → material_instance_path, parameters\n"
		"  set_skeletal_mesh_material→ skeletal_mesh_path, material_slot, material_path\n"
		"  set_actor_material        → actor_name, material_path [, material_slot]\n"
		"  get_material_info         → asset_path\n\n"
		"PARAMETERS OBJECT FORMAT:\n"
		"  {\"scalars\":{\"Roughness\":0.5},\"vectors\":{\"BaseColor\":{\"R\":1,\"G\":0,\"B\":0,\"A\":1}},\"textures\":{\"DiffuseMap\":\"/Game/Textures/T_Rock\"}}\n"
		"  Vector values use uppercase R/G/B/A."
	);

	// Parameters
	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("create_material | create_material_instance | set_material_parameters | set_skeletal_mesh_material | set_actor_material | get_material_info"), true));

	// create_material params
	Info.Parameters.Add(FMCPToolParameter(TEXT("blend_mode"), TEXT("string"),
		TEXT("Blend mode: opaque (default), translucent, masked, additive. For create_material.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("base_color"), TEXT("object"),
		TEXT("{\"r\":0.0,\"g\":0.15,\"b\":0.2} — BaseColor constant. Default white.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("roughness"), TEXT("number"),
		TEXT("Roughness constant 0–1. Default 0.5.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("metallic"), TEXT("number"),
		TEXT("Metallic constant 0–1. Default 0.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("opacity"), TEXT("number"),
		TEXT("Opacity constant 0–1 (only used for translucent/masked). Default 1.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("specular"), TEXT("number"),
		TEXT("Specular constant 0–1. Default 0.5.")));

	// create_material_instance params
	Info.Parameters.Add(FMCPToolParameter(TEXT("asset_name"), TEXT("string"),
		TEXT("Asset name for new material instance (no extension). Required for create_material_instance.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("parent_material"), TEXT("string"),
		TEXT("Asset path to parent material (e.g., '/Game/Materials/M_Rock'). Required for create_material_instance.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("package_path"), TEXT("string"),
		TEXT("Folder path for new asset (e.g., '/Game/Materials'). Default: '/Game/Materials'.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("parameters"), TEXT("object"),
		TEXT("Parameters to set: {\"scalars\":{\"Name\":value}, \"vectors\":{\"Name\":{\"R\":r,\"G\":g,\"B\":b,\"A\":a}}, \"textures\":{\"Name\":\"/Game/path\"}}.")));

	// set_material_parameters params
	Info.Parameters.Add(FMCPToolParameter(TEXT("material_instance_path"), TEXT("string"),
		TEXT("Asset path to an existing material instance. Required for set_material_parameters.")));

	// set_skeletal_mesh_material params
	Info.Parameters.Add(FMCPToolParameter(TEXT("skeletal_mesh_path"), TEXT("string"),
		TEXT("Asset path to skeletal mesh (e.g., '/Game/Characters/SK_Hero'). Required for set_skeletal_mesh_material.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("material_slot"), TEXT("integer"),
		TEXT("Zero-based material slot index. Required for set_skeletal_mesh_material. Use get_material_info to list slots.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("material_path"), TEXT("string"),
		TEXT("Asset path to material or material instance to assign. Required for set_skeletal_mesh_material.")));

	// set_actor_material params
	Info.Parameters.Add(FMCPToolParameter(TEXT("actor_name"), TEXT("string"),
		TEXT("Actor Outliner label or internal name. Required for set_actor_material.")));

	// get_material_info params
	Info.Parameters.Add(FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
		TEXT("Asset path to a material or material instance (e.g., '/Game/Materials/MI_Rock'). Required for get_material_info.")));

	Info.Annotations = FMCPToolAnnotations::Modifying();

	return Info;
}

FMCPToolResult FMCPTool_Material::Execute(const TSharedRef<FJsonObject>& Params)
{
	FString Operation;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("operation"), Operation, Error))
	{
		return Error.GetValue();
	}

	Operation = Operation.ToLower();

	if (Operation == TEXT("create_material"))
	{
		return ExecuteCreateMaterial(Params);
	}
	else if (Operation == TEXT("create_material_instance"))
	{
		return ExecuteCreateMaterialInstance(Params);
	}
	else if (Operation == TEXT("set_material_parameters"))
	{
		return ExecuteSetMaterialParameters(Params);
	}
	else if (Operation == TEXT("set_skeletal_mesh_material"))
	{
		return ExecuteSetSkeletalMeshMaterial(Params);
	}
	else if (Operation == TEXT("set_actor_material"))
	{
		return ExecuteSetActorMaterial(Params);
	}
	else if (Operation == TEXT("get_material_info"))
	{
		return ExecuteGetMaterialInfo(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: %s. Valid: create_material_instance, set_material_parameters, set_skeletal_mesh_material, set_actor_material, get_material_info"),
		*Operation));
}

FMCPToolResult FMCPTool_Material::ExecuteCreateMaterial(const TSharedRef<FJsonObject>& Params)
{
	// ── Required ────────────────────────────────────────────────────────────────
	FString AssetName;
	TOptional<FMCPToolResult> Error;
	if (!ExtractRequiredString(Params, TEXT("asset_name"), AssetName, Error))
		return Error.GetValue();

	// ── Optional ────────────────────────────────────────────────────────────────
	FString PackagePath = Params->HasField(TEXT("package_path"))
		? Params->GetStringField(TEXT("package_path"))
		: TEXT("/Game/Materials");
	if (!PackagePath.EndsWith(TEXT("/")))
		PackagePath += TEXT("/");

	FString BlendModeStr;
	Params->TryGetStringField(TEXT("blend_mode"), BlendModeStr);
	BlendModeStr = BlendModeStr.ToLower();

	// BaseColor (default white)
	double BaseR = 1.0, BaseG = 1.0, BaseB = 1.0;
	const TSharedPtr<FJsonObject>* BaseColorObj;
	if (Params->TryGetObjectField(TEXT("base_color"), BaseColorObj))
	{
		(*BaseColorObj)->TryGetNumberField(TEXT("r"), BaseR);
		(*BaseColorObj)->TryGetNumberField(TEXT("g"), BaseG);
		(*BaseColorObj)->TryGetNumberField(TEXT("b"), BaseB);
	}

	double Roughness = 0.5;  Params->TryGetNumberField(TEXT("roughness"), Roughness);
	double Metallic  = 0.0;  Params->TryGetNumberField(TEXT("metallic"),  Metallic);
	double Opacity   = 1.0;  Params->TryGetNumberField(TEXT("opacity"),   Opacity);
	double Specular  = 0.5;  Params->TryGetNumberField(TEXT("specular"),  Specular);

	// ── Create package ──────────────────────────────────────────────────────────
	FString FullPath = PackagePath + AssetName;
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *FullPath));

	// ── Create UMaterial via factory ────────────────────────────────────────────
	UMaterialFactoryNew* MatFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* Material = Cast<UMaterial>(
		MatFactory->FactoryCreateNew(
			UMaterial::StaticClass(), Package, FName(*AssetName),
			RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Material)
		return FMCPToolResult::Error(TEXT("Failed to create UMaterial asset"));

	// ── Blend mode ──────────────────────────────────────────────────────────────
	EBlendMode BlendMode = BLEND_Opaque;
	if      (BlendModeStr == TEXT("translucent")) BlendMode = BLEND_Translucent;
	else if (BlendModeStr == TEXT("masked"))      BlendMode = BLEND_Masked;
	else if (BlendModeStr == TEXT("additive"))    BlendMode = BLEND_Additive;
	Material->BlendMode = BlendMode;

	// Surface lighting gives better translucent shading quality
	if (BlendMode == BLEND_Translucent)
		Material->TranslucencyLightingMode = TLM_Surface;

	// ── Helper: add expression to material graph ─────────────────────────────────
	auto AddExpr = [&](UMaterialExpression* Expr, int32 X, int32 Y)
	{
		Expr->MaterialExpressionEditorX = X;
		Expr->MaterialExpressionEditorY = Y;
		Material->GetExpressionCollection().Expressions.Add(Expr);
	};

	// ── BaseColor ───────────────────────────────────────────────────────────────
	UMaterialExpressionConstant3Vector* ColorExpr = NewObject<UMaterialExpressionConstant3Vector>(Material);
	ColorExpr->Constant = FLinearColor((float)BaseR, (float)BaseG, (float)BaseB, 1.0f);
	AddExpr(ColorExpr, -400, 0);
	// In UE5.3+ material input pins live in editor-only data
	UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
	EditorData->BaseColor.Expression = ColorExpr;

	// ── Metallic ─────────────────────────────────────────────────────────────────
	UMaterialExpressionConstant* MetallicExpr = NewObject<UMaterialExpressionConstant>(Material);
	MetallicExpr->R = (float)Metallic;
	AddExpr(MetallicExpr, -400, 120);
	EditorData->Metallic.Expression = MetallicExpr;

	// ── Roughness ────────────────────────────────────────────────────────────────
	UMaterialExpressionConstant* RoughExpr = NewObject<UMaterialExpressionConstant>(Material);
	RoughExpr->R = (float)Roughness;
	AddExpr(RoughExpr, -400, 200);
	EditorData->Roughness.Expression = RoughExpr;

	// ── Specular ─────────────────────────────────────────────────────────────────
	UMaterialExpressionConstant* SpecExpr = NewObject<UMaterialExpressionConstant>(Material);
	SpecExpr->R = (float)Specular;
	AddExpr(SpecExpr, -400, 280);
	EditorData->Specular.Expression = SpecExpr;

	// ── Opacity (translucent) or OpacityMask (masked) ────────────────────────────
	if (BlendMode == BLEND_Translucent || BlendMode == BLEND_Masked)
	{
		UMaterialExpressionConstant* OpacityExpr = NewObject<UMaterialExpressionConstant>(Material);
		OpacityExpr->R = (float)Opacity;
		AddExpr(OpacityExpr, -400, 360);
		if (BlendMode == BLEND_Translucent)
			EditorData->Opacity.Expression = OpacityExpr;
		else
			EditorData->OpacityMask.Expression = OpacityExpr;
	}

	// ── Compile and save ─────────────────────────────────────────────────────────
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Material);

	FString PackageFileName = FPackageName::LongPackageNameToFilename(FullPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, Material, *PackageFileName, SaveArgs);
	if (!SaveResult.IsSuccessful())
		return FMCPToolResult::Error(FString::Printf(TEXT("Material created but failed to save: %s"), *FullPath));

	// ── Result ───────────────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), FullPath);
	ResultData->SetStringField(TEXT("blend_mode"), BlendModeStr.IsEmpty() ? TEXT("opaque") : BlendModeStr);
	ResultData->SetBoolField(TEXT("saved"), true);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created material '%s' (blend_mode=%s)"),
			*FullPath, BlendModeStr.IsEmpty() ? TEXT("opaque") : *BlendModeStr),
		ResultData);
}

FMCPToolResult FMCPTool_Material::ExecuteCreateMaterialInstance(const TSharedRef<FJsonObject>& Params)
{
	// Extract required params
	FString AssetName;
	FString ParentMaterialPath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractRequiredString(Params, TEXT("asset_name"), AssetName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("parent_material"), ParentMaterialPath, Error))
	{
		return Error.GetValue();
	}

	// Validate paths
	if (!ValidateBlueprintPathParam(ParentMaterialPath, Error))
	{
		return Error.GetValue();
	}

	// Get package path
	FString PackagePath = Params->HasField(TEXT("package_path"))
		? Params->GetStringField(TEXT("package_path"))
		: TEXT("/Game/Materials/");

	if (!ValidateBlueprintPathParam(PackagePath, Error))
	{
		return Error.GetValue();
	}

	// Ensure package path ends with /
	if (!PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath += TEXT("/");
	}

	// Load parent material
	UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialPath);
	if (!ParentMaterial)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load parent material: %s"), *ParentMaterialPath));
	}

	// Create the package
	FString FullPackagePath = PackagePath + AssetName;
	UPackage* Package = CreatePackage(*FullPackagePath);
	if (!Package)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *FullPackagePath));
	}

	// Create material instance using factory
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = ParentMaterial;

	UMaterialInstanceConstant* MatInst = Cast<UMaterialInstanceConstant>(
		Factory->FactoryCreateNew(
			UMaterialInstanceConstant::StaticClass(),
			Package,
			FName(*AssetName),
			RF_Public | RF_Standalone,
			nullptr,
			GWarn
		)
	);

	if (!MatInst)
	{
		return FMCPToolResult::Error(TEXT("Failed to create material instance"));
	}

	// Apply parameters if provided
	if (Params->HasField(TEXT("parameters")))
	{
		const TSharedPtr<FJsonObject>* ParamsObj;
		if (Params->TryGetObjectField(TEXT("parameters"), ParamsObj))
		{
			FString ParamError;
			if (!ApplyParametersFromJson(MatInst, *ParamsObj, ParamError))
			{
				// Material was created but parameters failed - report warning
				UE_LOG(LogUnrealClaude, Warning, TEXT("Material instance created but some parameters failed: %s"), *ParamError);
			}
		}
	}

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(MatInst);
	Package->MarkPackageDirty();

	// Save the asset
	FString PackageFileName = FPackageName::LongPackageNameToFilename(FullPackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, MatInst, *PackageFileName, SaveArgs);

	if (!SaveResult.IsSuccessful())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Material instance created but failed to save: %s"), *FullPackagePath));
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), FullPackagePath);
	ResultData->SetStringField(TEXT("asset_name"), AssetName);
	ResultData->SetStringField(TEXT("parent_material"), ParentMaterialPath);
	ResultData->SetBoolField(TEXT("saved"), true);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Created material instance: %s"), *FullPackagePath),
		ResultData
	);
}

FMCPToolResult FMCPTool_Material::ExecuteSetMaterialParameters(const TSharedRef<FJsonObject>& Params)
{
	FString MaterialInstancePath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractRequiredString(Params, TEXT("material_instance_path"), MaterialInstancePath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(MaterialInstancePath, Error))
	{
		return Error.GetValue();
	}

	// Load material instance
	UMaterialInstanceConstant* MatInst = LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialInstancePath);
	if (!MatInst)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load material instance: %s"), *MaterialInstancePath));
	}

	// Get parameters
	const TSharedPtr<FJsonObject>* ParamsObj;
	if (!Params->TryGetObjectField(TEXT("parameters"), ParamsObj))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: parameters"));
	}

	// Apply parameters
	FString ParamError;
	if (!ApplyParametersFromJson(MatInst, *ParamsObj, ParamError))
	{
		return FMCPToolResult::Error(ParamError);
	}

	// Mark dirty and post edit
	MatInst->PostEditChange();
	MatInst->MarkPackageDirty();

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material_instance"), MaterialInstancePath);
	ResultData->SetBoolField(TEXT("modified"), true);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Updated parameters on material instance: %s"), *MaterialInstancePath),
		ResultData
	);
}

FMCPToolResult FMCPTool_Material::ExecuteSetSkeletalMeshMaterial(const TSharedRef<FJsonObject>& Params)
{
	FString SkeletalMeshPath;
	FString MaterialPath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractRequiredString(Params, TEXT("skeletal_mesh_path"), SkeletalMeshPath, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("material_path"), MaterialPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(SkeletalMeshPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	// Get slot index
	int32 MaterialSlot = 0;
	if (Params->HasField(TEXT("material_slot")))
	{
		MaterialSlot = Params->GetIntegerField(TEXT("material_slot"));
		if (MaterialSlot < 0)
		{
			return FMCPToolResult::Error(TEXT("material_slot must be >= 0"));
		}
	}

	// Load skeletal mesh
	USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
	if (!SkeletalMesh)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load skeletal mesh: %s"), *SkeletalMeshPath));
	}

	// Load material
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
	}

	// Check slot bounds
	TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
	if (MaterialSlot >= Materials.Num())
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Material slot %d out of range. Skeletal mesh has %d material slots."),
			MaterialSlot, Materials.Num()));
	}

	// Store old material name for result
	FString OldMaterialName = Materials[MaterialSlot].MaterialInterface
		? Materials[MaterialSlot].MaterialInterface->GetName()
		: TEXT("None");

	// Set the material
	Materials[MaterialSlot].MaterialInterface = Material;

	// Notify and mark dirty
	SkeletalMesh->PostEditChange();
	SkeletalMesh->MarkPackageDirty();

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("skeletal_mesh"), SkeletalMeshPath);
	ResultData->SetNumberField(TEXT("slot"), MaterialSlot);
	ResultData->SetStringField(TEXT("old_material"), OldMaterialName);
	ResultData->SetStringField(TEXT("new_material"), Material->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set material slot %d on %s to %s"), MaterialSlot, *SkeletalMesh->GetName(), *Material->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_Material::ExecuteSetActorMaterial(const TSharedRef<FJsonObject>& Params)
{
	FString ActorName;
	FString MaterialPath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractActorName(Params, TEXT("actor_name"), ActorName, Error))
	{
		return Error.GetValue();
	}
	if (!ExtractRequiredString(Params, TEXT("material_path"), MaterialPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	// Get optional slot index
	int32 MaterialSlot = 0;
	if (Params->HasField(TEXT("material_slot")))
	{
		MaterialSlot = Params->GetIntegerField(TEXT("material_slot"));
		if (MaterialSlot < 0)
		{
			return FMCPToolResult::Error(TEXT("material_slot must be >= 0"));
		}
	}

	// Validate editor context
	UWorld* World = nullptr;
	if (auto EditorError = ValidateEditorContext(World))
	{
		return EditorError.GetValue();
	}

	// Find actor
	AActor* Actor = FindActorByNameOrLabel(World, ActorName);
	if (!Actor)
	{
		return ActorNotFoundError(ActorName);
	}

	// Find first mesh component on actor
	UMeshComponent* MeshComp = Actor->FindComponentByClass<UMeshComponent>();
	if (!MeshComp)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Actor '%s' has no mesh component"), *ActorName));
	}

	// Validate slot bounds
	int32 NumMaterials = MeshComp->GetNumMaterials();
	if (MaterialSlot >= NumMaterials)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Material slot %d out of range. Mesh component has %d material slots."),
			MaterialSlot, NumMaterials));
	}

	// Load material
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
	}

	// Store old material name
	UMaterialInterface* OldMaterial = MeshComp->GetMaterial(MaterialSlot);
	FString OldMaterialName = OldMaterial ? OldMaterial->GetName() : TEXT("None");

	// Set material on the component (runtime override)
	MeshComp->SetMaterial(MaterialSlot, Material);

	// Mark dirty
	MarkActorDirty(Actor);

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("actor"), Actor->GetName());
	ResultData->SetStringField(TEXT("component"), MeshComp->GetName());
	ResultData->SetStringField(TEXT("component_type"), MeshComp->GetClass()->GetName());
	ResultData->SetNumberField(TEXT("slot"), MaterialSlot);
	ResultData->SetStringField(TEXT("old_material"), OldMaterialName);
	ResultData->SetStringField(TEXT("new_material"), Material->GetName());

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set material slot %d on actor '%s' (%s) to %s"),
			MaterialSlot, *Actor->GetName(), *MeshComp->GetClass()->GetName(), *Material->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_Material::ExecuteGetMaterialInfo(const TSharedRef<FJsonObject>& Params)
{
	FString AssetPath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(AssetPath, Error))
	{
		return Error.GetValue();
	}

	// Load material
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
	if (!Material)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load material: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> ResultData = BuildMaterialInfoJson(Material);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Material info: %s"), *Material->GetName()),
		ResultData
	);
}

bool FMCPTool_Material::SetScalarParameter(UMaterialInstanceConstant* MatInst, const FString& ParamName, float Value, FString& OutError)
{
	if (!MatInst)
	{
		OutError = TEXT("Invalid material instance");
		return false;
	}

	MatInst->SetScalarParameterValueEditorOnly(FName(*ParamName), Value);
	return true;
}

bool FMCPTool_Material::SetVectorParameter(UMaterialInstanceConstant* MatInst, const FString& ParamName, const FLinearColor& Value, FString& OutError)
{
	if (!MatInst)
	{
		OutError = TEXT("Invalid material instance");
		return false;
	}

	MatInst->SetVectorParameterValueEditorOnly(FName(*ParamName), Value);
	return true;
}

bool FMCPTool_Material::SetTextureParameter(UMaterialInstanceConstant* MatInst, const FString& ParamName, const FString& TexturePath, FString& OutError)
{
	if (!MatInst)
	{
		OutError = TEXT("Invalid material instance");
		return false;
	}

	UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("Failed to load texture: %s"), *TexturePath);
		return false;
	}

	MatInst->SetTextureParameterValueEditorOnly(FName(*ParamName), Texture);
	return true;
}

bool FMCPTool_Material::ApplyParametersFromJson(UMaterialInstanceConstant* MatInst, const TSharedPtr<FJsonObject>& ParamsObj, FString& OutError)
{
	if (!MatInst || !ParamsObj.IsValid())
	{
		OutError = TEXT("Invalid material instance or parameters");
		return false;
	}

	bool bAllSuccess = true;
	TArray<FString> Errors;

	// Process scalar parameters
	const TSharedPtr<FJsonObject>* ScalarsObj;
	if (ParamsObj->TryGetObjectField(TEXT("scalars"), ScalarsObj))
	{
		for (const auto& Pair : (*ScalarsObj)->Values)
		{
			double Value = 0.0;
			if (Pair.Value->TryGetNumber(Value))
			{
				FString Error;
				if (!SetScalarParameter(MatInst, Pair.Key, static_cast<float>(Value), Error))
				{
					Errors.Add(Error);
					bAllSuccess = false;
				}
			}
		}
	}

	// Process vector parameters
	const TSharedPtr<FJsonObject>* VectorsObj;
	if (ParamsObj->TryGetObjectField(TEXT("vectors"), VectorsObj))
	{
		for (const auto& Pair : (*VectorsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* ColorObj;
			if (Pair.Value->TryGetObject(ColorObj))
			{
				FLinearColor Color;
				(*ColorObj)->TryGetNumberField(TEXT("r"), Color.R);
				(*ColorObj)->TryGetNumberField(TEXT("g"), Color.G);
				(*ColorObj)->TryGetNumberField(TEXT("b"), Color.B);
				Color.A = 1.0f;
				(*ColorObj)->TryGetNumberField(TEXT("a"), Color.A);

				FString Error;
				if (!SetVectorParameter(MatInst, Pair.Key, Color, Error))
				{
					Errors.Add(Error);
					bAllSuccess = false;
				}
			}
		}
	}

	// Process texture parameters
	const TSharedPtr<FJsonObject>* TexturesObj;
	if (ParamsObj->TryGetObjectField(TEXT("textures"), TexturesObj))
	{
		for (const auto& Pair : (*TexturesObj)->Values)
		{
			FString TexturePath;
			if (Pair.Value->TryGetString(TexturePath))
			{
				FString Error;
				if (!SetTextureParameter(MatInst, Pair.Key, TexturePath, Error))
				{
					Errors.Add(Error);
					bAllSuccess = false;
				}
			}
		}
	}

	if (!bAllSuccess)
	{
		OutError = FString::Join(Errors, TEXT("; "));
	}

	return bAllSuccess;
}

TSharedPtr<FJsonObject> FMCPTool_Material::BuildMaterialInfoJson(UMaterialInterface* Material)
{
	TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();

	Info->SetStringField(TEXT("name"), Material->GetName());
	Info->SetStringField(TEXT("path"), Material->GetPathName());
	Info->SetStringField(TEXT("class"), Material->GetClass()->GetName());

	// Check if it's a material instance
	if (UMaterialInstance* MatInst = Cast<UMaterialInstance>(Material))
	{
		Info->SetBoolField(TEXT("is_instance"), true);
		if (MatInst->Parent)
		{
			Info->SetStringField(TEXT("parent"), MatInst->Parent->GetPathName());
		}

		// Add scalar parameters
		TSharedPtr<FJsonObject> ScalarsObj = MakeShared<FJsonObject>();
		for (const FScalarParameterValue& Param : MatInst->ScalarParameterValues)
		{
			ScalarsObj->SetNumberField(Param.ParameterInfo.Name.ToString(), Param.ParameterValue);
		}
		Info->SetObjectField(TEXT("scalar_parameters"), ScalarsObj);

		// Add vector parameters
		TSharedPtr<FJsonObject> VectorsObj = MakeShared<FJsonObject>();
		for (const FVectorParameterValue& Param : MatInst->VectorParameterValues)
		{
			TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
			ColorObj->SetNumberField(TEXT("r"), Param.ParameterValue.R);
			ColorObj->SetNumberField(TEXT("g"), Param.ParameterValue.G);
			ColorObj->SetNumberField(TEXT("b"), Param.ParameterValue.B);
			ColorObj->SetNumberField(TEXT("a"), Param.ParameterValue.A);
			VectorsObj->SetObjectField(Param.ParameterInfo.Name.ToString(), ColorObj);
		}
		Info->SetObjectField(TEXT("vector_parameters"), VectorsObj);

		// Add texture parameters
		TSharedPtr<FJsonObject> TexturesObj = MakeShared<FJsonObject>();
		for (const FTextureParameterValue& Param : MatInst->TextureParameterValues)
		{
			FString TexturePath = Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None");
			TexturesObj->SetStringField(Param.ParameterInfo.Name.ToString(), TexturePath);
		}
		Info->SetObjectField(TEXT("texture_parameters"), TexturesObj);
	}
	else
	{
		Info->SetBoolField(TEXT("is_instance"), false);
	}

	return Info;
}

TArray<TSharedPtr<FJsonValue>> FMCPTool_Material::GetMaterialParameters(UMaterialInterface* Material)
{
	TArray<TSharedPtr<FJsonValue>> Params;

	if (!Material)
	{
		return Params;
	}

	// Get all parameter names from the material
	TArray<FMaterialParameterInfo> ScalarParams;
	TArray<FGuid> ScalarGuids;
	Material->GetAllScalarParameterInfo(ScalarParams, ScalarGuids);

	for (const FMaterialParameterInfo& ParamInfo : ScalarParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("scalar"));
		Params.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TArray<FMaterialParameterInfo> VectorParams;
	TArray<FGuid> VectorGuids;
	Material->GetAllVectorParameterInfo(VectorParams, VectorGuids);

	for (const FMaterialParameterInfo& ParamInfo : VectorParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("vector"));
		Params.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TArray<FMaterialParameterInfo> TextureParams;
	TArray<FGuid> TextureGuids;
	Material->GetAllTextureParameterInfo(TextureParams, TextureGuids);

	for (const FMaterialParameterInfo& ParamInfo : TextureParams)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("texture"));
		Params.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	return Params;
}
