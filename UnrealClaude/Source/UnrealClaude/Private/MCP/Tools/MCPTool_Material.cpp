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
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialParameters.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "MaterialEditingLibrary.h"
#include "SceneTypes.h"
#include "Engine/EngineTypes.h"
// UMaterialEditorOnlyData is declared in Materials/Material.h (already included)
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "EditorAssetLibrary.h"
#include "Dom/JsonValue.h"

// Forward declarations of file-local helpers defined in the anonymous namespace lower in this file,
// so the operation handlers (which appear earlier) can call them.
#if WITH_EDITOR
namespace
{
	bool LoadEditableTarget(const FString& Path, UMaterial*& OutMaterial, UMaterialFunction*& OutFunction);
	FMaterialExpressionCollection* EditableCollection(UMaterial* M, UMaterialFunction* F);
	UMaterialExpression* FindExprByGuid(UMaterial* M, UMaterialFunction* F, const FGuid& Id, int32& OutCount);
	bool FinalizeEdit(UMaterial* M, UMaterialFunction* F);
	UClass* ResolveExpressionClass(const FString& InName);
	bool ResolveMaterialProperty(const FString& Name, EMaterialProperty& Out);
	TSharedPtr<FJsonObject> BuildMaterialFunctionInfoJson(UMaterialFunction* Function);
}
#endif

FMCPToolInfo FMCPTool_Material::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("material");
	Info.Description = TEXT(
		"Material instance creation, parameter editing, and assignment for actors and meshes.\n\n"
		"OPERATION → REQUIRED PARAMS:\n"
		"  create_material           → asset_name [, package_path, blend_mode, base_color, roughness, metallic, opacity, specular]\n"
		"  create_material_instance  → asset_name, parent_material [, package_path, parameters]\n"
		"  set_material_parameters   → material_path, parameters  (material_path may be a base UMaterial OR an instance)\n"
		"  set_skeletal_mesh_material→ skeletal_mesh_path, material_slot, material_path\n"
		"  set_actor_material        → actor_name, material_path [, material_slot]\n"
		"  get_material_info         → asset_path\n"
		"  set_expression_value      → material_path, node_id, value   (edit a constant node; material OR material function)\n"
		"  repair_expression_ids     → material_path                  (make every node id unique)\n"
		"  add_expression            → material_path, expression_class [, pos_x, pos_y]\n"
		"  connect_expression        → material_path, from_node [, from_output], (to_node [, to_input] | to_property)\n"
		"  delete_expression         → material_path, node_id\n\n"
		"GRAPH EDITING (add/connect/delete) works on a base UMaterial OR a UMaterialFunction (material_path = the\n"
		"function asset). get_material_info also reads UMaterialFunctions (function_inputs/function_outputs with ids).\n"
		"expression_class: friendly name e.g. 'Constant','Constant3Vector','Add','Multiply','Lerp','Max','Min',\n"
		"'Clamp','Fresnel','OneMinus','Power','ScalarParameter'. node ids are the 'id' GUIDs from get_material_info.\n"
		"from_output/to_input default to \"\" (first pin). to_property (materials only) e.g. 'BaseColor','EmissiveColor';\n"
		"for a function, connect to a function-output node via to_node. After add, set a constant's value with\n"
		"set_expression_value. Function edits recompile dependent materials.\n\n"
		"set_expression_value edits a constant expression node (Constant / Constant2Vector / Constant3Vector /\n"
		"Constant4Vector) addressed by node_id — the 'id' GUID from get_material_info's connected_inputs tree.\n"
		"value is a number for Constant, or {\"r\":..,\"g\":..,\"b\":..,\"a\":..} for the vector constants. Recompiles + saves.\n"
		"If get_material_info reports has_duplicate_node_ids, some node ids are ambiguous (copy-paste artifact) and\n"
		"set_expression_value will refuse them — run repair_expression_ids first, then re-read for unique ids.\n\n"
		"get_material_info returns (base materials AND instances): name, path, class, is_instance, parent,\n"
		"blend_mode, shading_models[], two_sided, material_domain, usage_flags[], use_material_attributes,\n"
		"scalar/vector/texture/static_switch parameters (defaults for base materials, overrides for instances),\n"
		"connected_inputs{} (expression tree feeding each output: BaseColor/Opacity/Normal/Refraction/...),\n"
		"and custom_outputs[] (e.g. Single Layer Water scattering/absorption nodes).\n\n"
		"PARAMETERS OBJECT FORMAT:\n"
		"  {\"scalars\":{\"Roughness\":0.5},\"vectors\":{\"BaseColor\":{\"R\":1,\"G\":0,\"B\":0,\"A\":1}},\"textures\":{\"DiffuseMap\":\"/Game/Textures/T_Rock\"}}\n"
		"  Vector values use uppercase R/G/B/A.\n"
		"  For a BASE UMaterial, set_material_parameters sets parameter DEFAULTS on the parameter nodes (recompiles + saves)\n"
		"  and also accepts \"static_switches\":{\"UseDetailNormal\":true}. Names must match existing parameters (use\n"
		"  get_material_info to list them). Editing a base material affects ALL its instances."
	);

	// Parameters
	Info.Parameters.Add(FMCPToolParameter(TEXT("operation"), TEXT("string"),
		TEXT("create_material | create_material_instance | set_material_parameters | set_skeletal_mesh_material | set_actor_material | get_material_info | set_expression_value | repair_expression_ids | add_expression | connect_expression | delete_expression"), true));

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
		TEXT("Legacy alias for material_path (material instance only). Prefer material_path for set_material_parameters.")));

	// set_skeletal_mesh_material params
	Info.Parameters.Add(FMCPToolParameter(TEXT("skeletal_mesh_path"), TEXT("string"),
		TEXT("Asset path to skeletal mesh (e.g., '/Game/Characters/SK_Hero'). Required for set_skeletal_mesh_material.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("material_slot"), TEXT("integer"),
		TEXT("Zero-based material slot index. Required for set_skeletal_mesh_material. Use get_material_info to list slots.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("material_path"), TEXT("string"),
		TEXT("Asset path to a material/instance. For set_skeletal_mesh_material & set_actor_material: the material to assign. "
		     "For set_material_parameters: the base UMaterial OR instance to edit (base materials get parameter DEFAULTS set + recompiled).")));

	// set_actor_material params
	Info.Parameters.Add(FMCPToolParameter(TEXT("actor_name"), TEXT("string"),
		TEXT("Actor Outliner label or internal name. Required for set_actor_material.")));

	// get_material_info params
	Info.Parameters.Add(FMCPToolParameter(TEXT("asset_path"), TEXT("string"),
		TEXT("Asset path to a material or material instance (e.g., '/Game/Materials/MI_Rock'). Required for get_material_info.")));

	// set_expression_value params
	Info.Parameters.Add(FMCPToolParameter(TEXT("node_id"), TEXT("string"),
		TEXT("Expression node GUID (the 'id' field from get_material_info). Required for set_expression_value.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("value"), TEXT("any"),
		TEXT("New constant value. Number for Constant; {\"r\":..,\"g\":..,\"b\":..,\"a\":..} for Constant2/3/4Vector. Required for set_expression_value.")));

	// add_expression / connect_expression / delete_expression params (Phase 2c — graph topology)
	Info.Parameters.Add(FMCPToolParameter(TEXT("expression_class"), TEXT("string"),
		TEXT("Expression class to create (friendly name: 'Constant','Constant3Vector','Add','Multiply','Lerp','Max','Fresnel','OneMinus','Power','ScalarParameter',...). Required for add_expression.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("pos_x"), TEXT("integer"),
		TEXT("Graph X position for a new node (add_expression). Optional, default 0.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("pos_y"), TEXT("integer"),
		TEXT("Graph Y position for a new node (add_expression). Optional, default 0.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("from_node"), TEXT("string"),
		TEXT("Source expression node id (GUID) whose output is wired. Required for connect_expression.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("from_output"), TEXT("string"),
		TEXT("Source output pin name (default \"\" = first output, e.g. 'R'/'RGB' on a constant). For connect_expression.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("to_node"), TEXT("string"),
		TEXT("Target expression node id (GUID) to receive the connection. For connect_expression (use this OR to_property).")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("to_input"), TEXT("string"),
		TEXT("Target input pin name (default \"\" = first input, e.g. 'A'/'B' on Add/Lerp/Max). For connect_expression with to_node.")));
	Info.Parameters.Add(FMCPToolParameter(TEXT("to_property"), TEXT("string"),
		TEXT("Material output property to connect to (materials only): 'BaseColor','EmissiveColor','Normal','Roughness','Opacity','Refraction', etc. For connect_expression (use this OR to_node).")));

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
	else if (Operation == TEXT("set_expression_value"))
	{
		return ExecuteSetExpressionValue(Params);
	}
	else if (Operation == TEXT("repair_expression_ids"))
	{
		return ExecuteRepairExpressionIds(Params);
	}
	else if (Operation == TEXT("add_expression"))
	{
		return ExecuteAddExpression(Params);
	}
	else if (Operation == TEXT("connect_expression"))
	{
		return ExecuteConnectExpression(Params);
	}
	else if (Operation == TEXT("delete_expression"))
	{
		return ExecuteDeleteExpression(Params);
	}

	return FMCPToolResult::Error(FString::Printf(
		TEXT("Unknown operation: %s. Valid: create_material, create_material_instance, set_material_parameters, set_skeletal_mesh_material, set_actor_material, get_material_info, set_expression_value, repair_expression_ids, add_expression, connect_expression, delete_expression"),
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
	TOptional<FMCPToolResult> Error;

	// Resolve path: prefer generic `material_path`, fall back to legacy `material_instance_path`.
	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("material_instance_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or material_instance_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	// Get parameters
	const TSharedPtr<FJsonObject>* ParamsObj;
	if (!Params->TryGetObjectField(TEXT("parameters"), ParamsObj))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: parameters"));
	}

	// Load as a generic interface so we can handle both base materials and instances.
	UMaterialInterface* MatIface = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!MatIface)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
	}

	FString ParamError;
	bool bIsBaseMaterial = false;

	if (UMaterialInstanceConstant* MatInst = Cast<UMaterialInstanceConstant>(MatIface))
	{
		// Instance: override parent parameters (existing behavior).
		if (!ApplyParametersFromJson(MatInst, *ParamsObj, ParamError))
		{
			return FMCPToolResult::Error(ParamError);
		}
		MatInst->PostEditChange();
		MatInst->MarkPackageDirty();
	}
	else if (UMaterial* BaseMat = Cast<UMaterial>(MatIface))
	{
		// Base material: set parameter DEFAULTS on the parameter expression nodes (recompiles shaders).
		bIsBaseMaterial = true;
		if (!ApplyParametersToBaseMaterial(BaseMat, *ParamsObj, ParamError))
		{
			return FMCPToolResult::Error(ParamError);
		}
		BaseMat->PostEditChange();
		BaseMat->MarkPackageDirty();
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unsupported material type for editing: %s (expected base UMaterial or UMaterialInstanceConstant)"),
			*MatIface->GetClass()->GetName()));
	}

	// Save so the change persists across sessions.
	bool bSaved = false;
	if (UPackage* Package = MatIface->GetPackage())
	{
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		bSaved = UPackage::Save(Package, MatIface, *PackageFileName, SaveArgs).IsSuccessful();
	}

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetBoolField(TEXT("is_base_material"), bIsBaseMaterial);
	ResultData->SetBoolField(TEXT("modified"), true);
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Updated parameters on %s: %s%s"),
			bIsBaseMaterial ? TEXT("base material") : TEXT("material instance"),
			*MaterialPath,
			bSaved ? TEXT("") : TEXT(" (NOT saved — file may be read-only)")),
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

	// Load as a generic object so we can handle materials, instances, AND material functions.
	UObject* Obj = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Obj)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> ResultData;
	if (UMaterialInterface* Material = Cast<UMaterialInterface>(Obj))
	{
		ResultData = BuildMaterialInfoJson(Material);
	}
#if WITH_EDITOR
	else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Obj))
	{
		ResultData = BuildMaterialFunctionInfoJson(Function);
	}
#endif
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Asset is not a material or material function: %s (%s)"), *AssetPath, *Obj->GetClass()->GetName()));
	}

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Material info: %s"), *Obj->GetName()),
		ResultData
	);
}

FMCPToolResult FMCPTool_Material::ExecuteSetExpressionValue(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	// Resolve material path (base UMaterial only — instances have no expression graph of their own).
	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or asset_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	FString NodeIdStr;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeIdStr, Error))
	{
		return Error.GetValue();
	}
	FGuid NodeId;
	if (!FGuid::Parse(NodeIdStr, NodeId) || !NodeId.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Invalid node_id (expected a GUID from get_material_info's 'id' field): %s"), *NodeIdStr));
	}

	const TSharedPtr<FJsonValue> ValueVal = Params->TryGetField(TEXT("value"));
	if (!ValueVal.IsValid())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: value"));
	}

#if WITH_EDITOR
	UMaterial* M = nullptr;
	UMaterialFunction* F = nullptr;
	if (!LoadEditableTarget(MaterialPath, M, F))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load a base material or material function: %s (material instances have no editable graph)"), *MaterialPath));
	}

	// Locate the target expression by GUID. Distinct expressions can share a GUID (copy-paste
	// authoring artifact); editing an arbitrary one would be a silent wrong-target, so refuse.
	int32 MatchCount = 0;
	UMaterialExpression* Target = FindExprByGuid(M, F, NodeId, MatchCount);
	if (MatchCount == 0)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("No expression with node_id %s found on %s"), *NodeIdStr, *MaterialPath));
	}
	if (MatchCount > 1)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Ambiguous node_id %s — %d expressions share this GUID (duplicate MaterialExpressionGuids, usually from copy-paste). Run operation 'repair_expression_ids' on this asset, then re-read with get_material_info for unique ids."),
			*NodeIdStr, MatchCount));
	}

	// Apply by concrete constant type.
	FString OldValue, NewValue;
	Target->Modify();

	if (UMaterialExpressionConstant* C = Cast<UMaterialExpressionConstant>(Target))
	{
		double V = 0.0;
		if (!ValueVal->TryGetNumber(V))
		{
			return FMCPToolResult::Error(TEXT("Constant expects a numeric 'value' (e.g. 0.5)"));
		}
		OldValue = FString::SanitizeFloat(C->R);
		C->R = (float)V;
		NewValue = FString::SanitizeFloat(C->R);
	}
	else if (UMaterialExpressionConstant2Vector* C2 = Cast<UMaterialExpressionConstant2Vector>(Target))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!ValueVal->TryGetObject(Obj))
		{
			return FMCPToolResult::Error(TEXT("Constant2Vector expects an object 'value' {\"r\":..,\"g\":..}"));
		}
		double R = C2->R, G = C2->G;
		(*Obj)->TryGetNumberField(TEXT("r"), R);
		(*Obj)->TryGetNumberField(TEXT("g"), G);
		OldValue = FString::Printf(TEXT("(%g,%g)"), C2->R, C2->G);
		C2->R = (float)R; C2->G = (float)G;
		NewValue = FString::Printf(TEXT("(%g,%g)"), C2->R, C2->G);
	}
	else if (UMaterialExpressionConstant3Vector* C3 = Cast<UMaterialExpressionConstant3Vector>(Target))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!ValueVal->TryGetObject(Obj))
		{
			return FMCPToolResult::Error(TEXT("Constant3Vector expects an object 'value' {\"r\":..,\"g\":..,\"b\":..}"));
		}
		FLinearColor Col = C3->Constant;
		(*Obj)->TryGetNumberField(TEXT("r"), Col.R);
		(*Obj)->TryGetNumberField(TEXT("g"), Col.G);
		(*Obj)->TryGetNumberField(TEXT("b"), Col.B);
		(*Obj)->TryGetNumberField(TEXT("a"), Col.A);
		OldValue = C3->Constant.ToString();
		C3->Constant = Col;
		NewValue = C3->Constant.ToString();
	}
	else if (UMaterialExpressionConstant4Vector* C4 = Cast<UMaterialExpressionConstant4Vector>(Target))
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (!ValueVal->TryGetObject(Obj))
		{
			return FMCPToolResult::Error(TEXT("Constant4Vector expects an object 'value' {\"r\":..,\"g\":..,\"b\":..,\"a\":..}"));
		}
		FLinearColor Col = C4->Constant;
		(*Obj)->TryGetNumberField(TEXT("r"), Col.R);
		(*Obj)->TryGetNumberField(TEXT("g"), Col.G);
		(*Obj)->TryGetNumberField(TEXT("b"), Col.B);
		(*Obj)->TryGetNumberField(TEXT("a"), Col.A);
		OldValue = C4->Constant.ToString();
		C4->Constant = Col;
		NewValue = C4->Constant.ToString();
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Expression '%s' (id %s) is not an editable constant. Supported: Constant, Constant2Vector, Constant3Vector, Constant4Vector. For parameter nodes use set_material_parameters."),
			*Target->GetClass()->GetName(), *NodeIdStr));
	}

	Target->PostEditChange();
	const bool bSaved = FinalizeEdit(M, F);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetStringField(TEXT("node_id"), NodeIdStr);
	ResultData->SetStringField(TEXT("class"), Target->GetClass()->GetName());
	ResultData->SetStringField(TEXT("old_value"), OldValue);
	ResultData->SetStringField(TEXT("new_value"), NewValue);
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Set %s (id %s): %s -> %s%s"),
			*Target->GetClass()->GetName(), *NodeIdStr, *OldValue, *NewValue,
			bSaved ? TEXT("") : TEXT(" (NOT saved — file may be read-only)")),
		ResultData);
#else
	return FMCPToolResult::Error(TEXT("set_expression_value requires an editor build"));
#endif
}

FMCPToolResult FMCPTool_Material::ExecuteRepairExpressionIds(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or asset_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

#if WITH_EDITOR
	UMaterial* M = nullptr;
	UMaterialFunction* F = nullptr;
	if (!LoadEditableTarget(MaterialPath, M, F))
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load a base material or material function: %s"), *MaterialPath));
	}
	FMaterialExpressionCollection* Coll = EditableCollection(M, F);

	// Keep the first occurrence of each GUID; force-regenerate any later collision to a fresh unique id.
	TSet<FGuid> Seen;
	int32 Repaired = 0;
	for (const TObjectPtr<UMaterialExpression>& ExprPtr : Coll->Expressions)
	{
		UMaterialExpression* E = ExprPtr;
		if (!E)
		{
			continue;
		}
		E->UpdateMaterialExpressionGuid(false, false);
		const FGuid Id = E->GetMaterialExpressionId();
		if (Seen.Contains(Id))
		{
			E->UpdateMaterialExpressionGuid(true, true); // force a new, unique GUID
			Seen.Add(E->GetMaterialExpressionId());
			++Repaired;
		}
		else
		{
			Seen.Add(Id);
		}
	}

	const bool bSaved = (Repaired > 0) ? FinalizeEdit(M, F) : false;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetNumberField(TEXT("duplicates_repaired"), Repaired);
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Repaired %d duplicate node id(s) on %s%s"),
			Repaired, *MaterialPath,
			(Repaired > 0 && !bSaved) ? TEXT(" (NOT saved — file may be read-only)") : TEXT("")),
		ResultData);
#else
	return FMCPToolResult::Error(TEXT("repair_expression_ids requires an editor build"));
#endif
}

FMCPToolResult FMCPTool_Material::ExecuteAddExpression(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or asset_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	FString ClassName;
	if (!ExtractRequiredString(Params, TEXT("expression_class"), ClassName, Error))
	{
		return Error.GetValue();
	}

	int32 PosX = 0, PosY = 0;
	if (Params->HasField(TEXT("pos_x"))) { PosX = Params->GetIntegerField(TEXT("pos_x")); }
	if (Params->HasField(TEXT("pos_y"))) { PosY = Params->GetIntegerField(TEXT("pos_y")); }

#if WITH_EDITOR
	UMaterial* M = nullptr;
	UMaterialFunction* F = nullptr;
	if (!LoadEditableTarget(MaterialPath, M, F))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load a base material or material function: %s"), *MaterialPath));
	}

	UClass* Cls = ResolveExpressionClass(ClassName);
	if (!Cls)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Unknown expression class '%s' (try e.g. 'Constant', 'Constant3Vector', 'Add', 'Multiply', 'Lerp', 'Max', 'Min', 'Clamp', 'Fresnel', 'OneMinus', 'Power', 'ScalarParameter')"),
			*ClassName));
	}

	UMaterialExpression* NewExpr = M
		? UMaterialEditingLibrary::CreateMaterialExpression(M, Cls, PosX, PosY)
		: UMaterialEditingLibrary::CreateMaterialExpressionInFunction(F, Cls, PosX, PosY);
	if (!NewExpr)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create expression '%s'"), *Cls->GetName()));
	}

	// Guarantee a fresh, unique node id for addressing.
	NewExpr->UpdateMaterialExpressionGuid(true, true);
	const FString NewId = NewExpr->GetMaterialExpressionId().ToString();

	const bool bSaved = FinalizeEdit(M, F);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetStringField(TEXT("node_id"), NewId);
	ResultData->SetStringField(TEXT("class"), NewExpr->GetClass()->GetName());
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Added %s (id %s) to %s%s"),
			*NewExpr->GetClass()->GetName(), *NewId, *MaterialPath,
			bSaved ? TEXT("") : TEXT(" (NOT saved — file may be read-only)")),
		ResultData);
#else
	return FMCPToolResult::Error(TEXT("add_expression requires an editor build"));
#endif
}

FMCPToolResult FMCPTool_Material::ExecuteConnectExpression(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or asset_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	FString FromIdStr;
	if (!ExtractRequiredString(Params, TEXT("from_node"), FromIdStr, Error))
	{
		return Error.GetValue();
	}
	FGuid FromId;
	if (!FGuid::Parse(FromIdStr, FromId) || !FromId.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Invalid from_node GUID: %s"), *FromIdStr));
	}

	FString FromOutput;
	Params->TryGetStringField(TEXT("from_output"), FromOutput); // default "" = first output

#if WITH_EDITOR
	UMaterial* M = nullptr;
	UMaterialFunction* F = nullptr;
	if (!LoadEditableTarget(MaterialPath, M, F))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load a base material or material function: %s"), *MaterialPath));
	}

	int32 FromCount = 0;
	UMaterialExpression* FromExpr = FindExprByGuid(M, F, FromId, FromCount);
	if (FromCount == 0)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("from_node %s not found on %s"), *FromIdStr, *MaterialPath));
	}
	if (FromCount > 1)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Ambiguous from_node %s (%d matches) — run repair_expression_ids first."), *FromIdStr, FromCount));
	}

	FString ToProperty;
	const bool bHasProperty = Params->TryGetStringField(TEXT("to_property"), ToProperty) && !ToProperty.IsEmpty();

	bool bOk = false;
	FString Desc;
	if (bHasProperty)
	{
		if (!M)
		{
			return FMCPToolResult::Error(TEXT("to_property is only valid for materials. For a material function, connect to a function output node via to_node."));
		}
		EMaterialProperty Prop;
		if (!ResolveMaterialProperty(ToProperty, Prop))
		{
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Unknown material property '%s' (e.g. BaseColor, EmissiveColor, Normal, Roughness, Metallic, Specular, Opacity, Refraction, WorldPositionOffset)"), *ToProperty));
		}
		bOk = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, FromOutput, Prop);
		Desc = FString::Printf(TEXT("%s.%s -> property %s"),
			*FromExpr->GetClass()->GetName(), FromOutput.IsEmpty() ? TEXT("(default)") : *FromOutput, *ToProperty);
	}
	else
	{
		FString ToIdStr;
		if (!ExtractRequiredString(Params, TEXT("to_node"), ToIdStr, Error))
		{
			return Error.GetValue();
		}
		FGuid ToId;
		if (!FGuid::Parse(ToIdStr, ToId) || !ToId.IsValid())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Invalid to_node GUID: %s"), *ToIdStr));
		}
		int32 ToCount = 0;
		UMaterialExpression* ToExpr = FindExprByGuid(M, F, ToId, ToCount);
		if (ToCount == 0)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("to_node %s not found on %s"), *ToIdStr, *MaterialPath));
		}
		if (ToCount > 1)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Ambiguous to_node %s (%d matches) — run repair_expression_ids first."), *ToIdStr, ToCount));
		}

		FString ToInput;
		Params->TryGetStringField(TEXT("to_input"), ToInput); // default "" = first input
		bOk = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpr, FromOutput, ToExpr, ToInput);
		Desc = FString::Printf(TEXT("%s.%s -> %s.%s"),
			*FromExpr->GetClass()->GetName(), FromOutput.IsEmpty() ? TEXT("(default)") : *FromOutput,
			*ToExpr->GetClass()->GetName(), ToInput.IsEmpty() ? TEXT("(default)") : *ToInput);
	}

	if (!bOk)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Connection failed (%s) — verify the output/input names exist (use get_material_info to list node inputs; default \"\" picks the first)."), *Desc));
	}

	const bool bSaved = FinalizeEdit(M, F);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetStringField(TEXT("connection"), Desc);
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Connected %s%s"), *Desc, bSaved ? TEXT("") : TEXT(" (NOT saved — file may be read-only)")),
		ResultData);
#else
	return FMCPToolResult::Error(TEXT("connect_expression requires an editor build"));
#endif
}

FMCPToolResult FMCPTool_Material::ExecuteDeleteExpression(const TSharedRef<FJsonObject>& Params)
{
	TOptional<FMCPToolResult> Error;

	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), MaterialPath);
	}
	if (MaterialPath.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: material_path (or asset_path)"));
	}
	if (!ValidateBlueprintPathParam(MaterialPath, Error))
	{
		return Error.GetValue();
	}

	FString NodeIdStr;
	if (!ExtractRequiredString(Params, TEXT("node_id"), NodeIdStr, Error))
	{
		return Error.GetValue();
	}
	FGuid NodeId;
	if (!FGuid::Parse(NodeIdStr, NodeId) || !NodeId.IsValid())
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Invalid node_id GUID: %s"), *NodeIdStr));
	}

#if WITH_EDITOR
	UMaterial* M = nullptr;
	UMaterialFunction* F = nullptr;
	if (!LoadEditableTarget(MaterialPath, M, F))
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load a base material or material function: %s"), *MaterialPath));
	}

	int32 MatchCount = 0;
	UMaterialExpression* Target = FindExprByGuid(M, F, NodeId, MatchCount);
	if (MatchCount == 0)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("No expression with node_id %s found on %s"), *NodeIdStr, *MaterialPath));
	}
	if (MatchCount > 1)
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Ambiguous node_id %s (%d matches) — run repair_expression_ids first."), *NodeIdStr, MatchCount));
	}

	const FString DeletedClass = Target->GetClass()->GetName();
	if (M)
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(M, Target);
	}
	else
	{
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(F, Target);
	}

	const bool bSaved = FinalizeEdit(M, F);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("material"), MaterialPath);
	ResultData->SetStringField(TEXT("deleted_node_id"), NodeIdStr);
	ResultData->SetStringField(TEXT("class"), DeletedClass);
	ResultData->SetBoolField(TEXT("saved"), bSaved);

	return FMCPToolResult::Success(
		FString::Printf(TEXT("Deleted %s (id %s) from %s%s"),
			*DeletedClass, *NodeIdStr, *MaterialPath, bSaved ? TEXT("") : TEXT(" (NOT saved — file may be read-only)")),
		ResultData);
#else
	return FMCPToolResult::Error(TEXT("delete_expression requires an editor build"));
#endif
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

bool FMCPTool_Material::ApplyParametersToBaseMaterial(UMaterial* Material, const TSharedPtr<FJsonObject>& ParamsObj, FString& OutError)
{
	if (!Material || !ParamsObj.IsValid())
	{
		OutError = TEXT("Invalid material or parameters");
		return false;
	}

#if WITH_EDITOR
	// Gather requested values keyed by parameter name. Each carries the expected type so we can
	// reject a value aimed at the wrong kind of parameter (e.g. a scalar sent to a vector param).
	struct FRequestedParam
	{
		FMaterialParameterValue Value;
		bool bApplied = false;
		bool bTypeMismatch = false;
	};
	TMap<FName, FRequestedParam> Requests;
	TArray<FString> Errors;

	// Scalars
	const TSharedPtr<FJsonObject>* ScalarsObj;
	if (ParamsObj->TryGetObjectField(TEXT("scalars"), ScalarsObj))
	{
		for (const auto& Pair : (*ScalarsObj)->Values)
		{
			double Value = 0.0;
			if (Pair.Value->TryGetNumber(Value))
			{
				Requests.Add(FName(*Pair.Key), FRequestedParam{ FMaterialParameterValue((float)Value) });
			}
		}
	}

	// Vectors (RGBA)
	const TSharedPtr<FJsonObject>* VectorsObj;
	if (ParamsObj->TryGetObjectField(TEXT("vectors"), VectorsObj))
	{
		for (const auto& Pair : (*VectorsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* ColorObj;
			if (Pair.Value->TryGetObject(ColorObj))
			{
				FLinearColor Color(0, 0, 0, 1);
				(*ColorObj)->TryGetNumberField(TEXT("r"), Color.R);
				(*ColorObj)->TryGetNumberField(TEXT("g"), Color.G);
				(*ColorObj)->TryGetNumberField(TEXT("b"), Color.B);
				(*ColorObj)->TryGetNumberField(TEXT("a"), Color.A);
				Requests.Add(FName(*Pair.Key), FRequestedParam{ FMaterialParameterValue(Color) });
			}
		}
	}

	// Static switches
	const TSharedPtr<FJsonObject>* SwitchesObj;
	if (ParamsObj->TryGetObjectField(TEXT("static_switches"), SwitchesObj))
	{
		for (const auto& Pair : (*SwitchesObj)->Values)
		{
			bool bValue = false;
			if (Pair.Value->TryGetBool(bValue))
			{
				Requests.Add(FName(*Pair.Key), FRequestedParam{ FMaterialParameterValue(bValue) });
			}
		}
	}

	// Textures (load by path)
	const TSharedPtr<FJsonObject>* TexturesObj;
	if (ParamsObj->TryGetObjectField(TEXT("textures"), TexturesObj))
	{
		for (const auto& Pair : (*TexturesObj)->Values)
		{
			FString TexturePath;
			if (Pair.Value->TryGetString(TexturePath))
			{
				UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
				if (!Texture)
				{
					Errors.Add(FString::Printf(TEXT("Failed to load texture '%s' for parameter '%s'"), *TexturePath, *Pair.Key));
					continue;
				}
				Requests.Add(FName(*Pair.Key), FRequestedParam{ FMaterialParameterValue(Texture) });
			}
		}
	}

	if (Requests.Num() == 0 && Errors.Num() == 0)
	{
		OutError = TEXT("No applicable parameters provided (expected scalars / vectors / textures / static_switches)");
		return false;
	}

	// Apply to every matching parameter expression. UE allows duplicate-named parameter nodes whose
	// defaults must stay in sync, so we set ALL matches rather than stopping at the first.
	for (const TObjectPtr<UMaterialExpression>& ExprPtr : Material->GetExpressionCollection().Expressions)
	{
		UMaterialExpression* Expr = ExprPtr;
		if (!Expr || !Expr->HasAParameterName())
		{
			continue;
		}

		const FName PName = Expr->GetParameterName();
		FRequestedParam* Req = Requests.Find(PName);
		if (!Req)
		{
			continue;
		}

		FMaterialParameterMetadata Meta;
		if (!Expr->GetParameterValue(Meta))
		{
			continue;
		}
		if (Meta.Value.Type != Req->Value.Type)
		{
			Req->bTypeMismatch = true;
			Errors.AddUnique(FString::Printf(TEXT("Type mismatch for parameter '%s' (material expects a different parameter type)"), *PName.ToString()));
			continue;
		}

		Meta.Value = Req->Value;
		Expr->SetParameterValue(PName, Meta, EMaterialExpressionSetParameterValueFlags::SendPostEditChangeProperty);
		Req->bApplied = true;
	}

	// Report any requested names that never matched a parameter expression.
	for (const auto& Pair : Requests)
	{
		if (!Pair.Value.bApplied && !Pair.Value.bTypeMismatch)
		{
			Errors.AddUnique(FString::Printf(TEXT("Parameter '%s' not found on material"), *Pair.Key.ToString()));
		}
	}

	if (Errors.Num() > 0)
	{
		OutError = FString::Join(Errors, TEXT("; "));
		return false;
	}

	return true;
#else
	OutError = TEXT("Base-material parameter editing requires an editor build");
	return false;
#endif
}

// ── Local helpers for material introspection ─────────────────────────────────
namespace
{
	/** Authored enum value name with the type prefix (e.g. "BLEND_") stripped: "Opaque", "SingleLayerWater". */
	FString EnumValueName(const UEnum* EnumClass, int64 Value)
	{
		if (!EnumClass)
		{
			return FString::Printf(TEXT("%lld"), Value);
		}
		FString Name = EnumClass->GetNameStringByValue(Value);
		if (Name.IsEmpty())
		{
			return FString::Printf(TEXT("%lld"), Value);
		}
		int32 UnderscoreIdx;
		if (Name.FindChar(TEXT('_'), UnderscoreIdx))
		{
			Name = Name.RightChop(UnderscoreIdx + 1);
		}
		return Name;
	}

	/** Read all scalar/vector/texture/static-switch parameters (defaults for a base UMaterial,
	 *  overrides for an instance) into the result object. Works for both base materials and instances. */
	void AddMaterialParameters(UMaterialInterface* Material, const TSharedPtr<FJsonObject>& Info)
	{
		// Scalars
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			Material->GetAllScalarParameterInfo(Infos, Guids);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const FMaterialParameterInfo& PInfo : Infos)
			{
				float Value = 0.0f;
				if (Material->GetScalarParameterValue(PInfo, Value))
				{
					Obj->SetNumberField(PInfo.Name.ToString(), Value);
				}
			}
			Info->SetObjectField(TEXT("scalar_parameters"), Obj);
		}

		// Vectors
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			Material->GetAllVectorParameterInfo(Infos, Guids);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const FMaterialParameterInfo& PInfo : Infos)
			{
				FLinearColor Value;
				if (Material->GetVectorParameterValue(PInfo, Value))
				{
					TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
					ColorObj->SetNumberField(TEXT("r"), Value.R);
					ColorObj->SetNumberField(TEXT("g"), Value.G);
					ColorObj->SetNumberField(TEXT("b"), Value.B);
					ColorObj->SetNumberField(TEXT("a"), Value.A);
					Obj->SetObjectField(PInfo.Name.ToString(), ColorObj);
				}
			}
			Info->SetObjectField(TEXT("vector_parameters"), Obj);
		}

		// Textures
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			Material->GetAllTextureParameterInfo(Infos, Guids);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const FMaterialParameterInfo& PInfo : Infos)
			{
				UTexture* Texture = nullptr;
				if (Material->GetTextureParameterValue(PInfo, Texture))
				{
					Obj->SetStringField(PInfo.Name.ToString(), Texture ? Texture->GetPathName() : TEXT("None"));
				}
			}
			Info->SetObjectField(TEXT("texture_parameters"), Obj);
		}

		// Static switches
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			Material->GetAllStaticSwitchParameterInfo(Infos, Guids);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const FMaterialParameterInfo& PInfo : Infos)
			{
				bool bValue = false; FGuid Guid;
				if (Material->GetStaticSwitchParameterValue(PInfo, bValue, Guid))
				{
					Obj->SetBoolField(PInfo.Name.ToString(), bValue);
				}
			}
			Info->SetObjectField(TEXT("static_switch_parameters"), Obj);
		}
	}

#if WITH_EDITOR
	/** Recursively describe an expression node (class + caption + parameter name) and its connected
	 *  inputs, bounded by MaxDepth so we report "what drives this output" without walking the whole graph. */
	TSharedPtr<FJsonObject> DescribeExpression(UMaterialExpression* Expr, int32 Depth, int32 MaxDepth)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Expr)
		{
			return Obj;
		}

		// Stable node handle for editing (generate-if-missing; doesn't dirty the package on read).
		Expr->UpdateMaterialExpressionGuid(false, false);
		Obj->SetStringField(TEXT("id"), Expr->GetMaterialExpressionId().ToString());
		Obj->SetStringField(TEXT("class"), Expr->GetClass()->GetName());

		TArray<FString> Captions;
		Expr->GetCaption(Captions);
		if (Captions.Num() > 0)
		{
			Obj->SetStringField(TEXT("caption"), FString::Join(Captions, TEXT(" ")));
		}

		if (Expr->HasAParameterName())
		{
			Obj->SetStringField(TEXT("parameter"), Expr->GetParameterName().ToString());
		}

		if (Depth < MaxDepth)
		{
			TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
			for (FExpressionInputIterator It{ Expr }; It; ++It)
			{
				if (It.Input && It.Input->Expression)
				{
					const FName InputName = Expr->GetInputName(It.Index);
					FString Key = InputName.IsNone()
						? FString::Printf(TEXT("Input%d"), It.Index)
						: InputName.ToString();
					Inputs->SetObjectField(Key, DescribeExpression(It.Input->Expression, Depth + 1, MaxDepth));
				}
			}
			if (Inputs->Values.Num() > 0)
			{
				Obj->SetObjectField(TEXT("inputs"), Inputs);
			}
		}

		return Obj;
	}

	/** For each standard material output (BaseColor, Opacity, Normal, Refraction, ...) report the
	 *  expression tree feeding it. Answers "what drives this output" for a base UMaterial. */
	void AddConnectedInputs(UMaterial* BaseMat, const TSharedPtr<FJsonObject>& Info)
	{
		static const EMaterialProperty Props[] = {
			MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy,
			MP_EmissiveColor, MP_Opacity, MP_OpacityMask, MP_Normal, MP_Tangent,
			MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion,
			MP_Refraction, MP_PixelDepthOffset, MP_Displacement, MP_MaterialAttributes
		};

		const UEnum* PropEnum = StaticEnum<EMaterialProperty>();
		TSharedPtr<FJsonObject> Connected = MakeShared<FJsonObject>();
		for (EMaterialProperty Prop : Props)
		{
			FExpressionInput* In = BaseMat->GetExpressionInputForProperty(Prop);
			if (In && In->Expression)
			{
				Connected->SetObjectField(EnumValueName(PropEnum, (int64)Prop), DescribeExpression(In->Expression, 0, 3));
			}
		}
		Info->SetObjectField(TEXT("connected_inputs"), Connected);
	}

	/** Report custom-output expression nodes (Single Layer Water, Clear Coat Bottom Normal, etc.)
	 *  and the expression trees feeding their inputs. This is where Single Layer Water absorption /
	 *  scattering coefficients live. */
	void AddCustomOutputs(UMaterial* BaseMat, const TSharedPtr<FJsonObject>& Info)
	{
		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : BaseMat->GetExpressionCollection().Expressions)
		{
			UMaterialExpression* Expr = ExprPtr;
			UMaterialExpressionCustomOutput* CO = Cast<UMaterialExpressionCustomOutput>(Expr);
			if (!CO)
			{
				continue;
			}

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			CO->UpdateMaterialExpressionGuid(false, false);
			O->SetStringField(TEXT("id"), CO->GetMaterialExpressionId().ToString());
			O->SetStringField(TEXT("class"), CO->GetClass()->GetName());
			O->SetStringField(TEXT("name"), CO->GetDisplayName());

			TSharedPtr<FJsonObject> Inputs = MakeShared<FJsonObject>();
			for (FExpressionInputIterator It{ CO }; It; ++It)
			{
				if (It.Input && It.Input->Expression)
				{
					const FName InputName = CO->GetInputName(It.Index);
					FString Key = InputName.IsNone()
						? FString::Printf(TEXT("Input%d"), It.Index)
						: InputName.ToString();
					Inputs->SetObjectField(Key, DescribeExpression(It.Input->Expression, 0, 2));
				}
			}
			if (Inputs->Values.Num() > 0)
			{
				O->SetObjectField(TEXT("inputs"), Inputs);
			}

			Outputs.Add(MakeShared<FJsonValueObject>(O));
		}
		if (Outputs.Num() > 0)
		{
			Info->SetArrayField(TEXT("custom_outputs"), Outputs);
		}
	}

	// ── Material function (UMaterialFunction) introspection ──────────────────────
	TSharedPtr<FJsonObject> BuildMaterialFunctionInfoJson(UMaterialFunction* Function)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), Function->GetName());
		Info->SetStringField(TEXT("path"), Function->GetPathName());
		Info->SetStringField(TEXT("class"), Function->GetClass()->GetName());
		Info->SetBoolField(TEXT("is_function"), true);

		const UEnum* InTypeEnum = StaticEnum<EFunctionInputType>();
		TArray<TSharedPtr<FJsonValue>> InputsArr;
		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Function->GetExpressionCollection().Expressions)
		{
			UMaterialExpression* E = ExprPtr;
			if (!E)
			{
				continue;
			}
			if (UMaterialExpressionFunctionInput* FI = Cast<UMaterialExpressionFunctionInput>(E))
			{
				FI->UpdateMaterialExpressionGuid(false, false);
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("id"), FI->GetMaterialExpressionId().ToString());
				O->SetStringField(TEXT("name"), FI->InputName.ToString());
				O->SetStringField(TEXT("type"), EnumValueName(InTypeEnum, (int64)FI->InputType.GetValue()));
				InputsArr.Add(MakeShared<FJsonValueObject>(O));
			}
			else if (UMaterialExpressionFunctionOutput* FO = Cast<UMaterialExpressionFunctionOutput>(E))
			{
				FO->UpdateMaterialExpressionGuid(false, false);
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("id"), FO->GetMaterialExpressionId().ToString());
				O->SetStringField(TEXT("name"), FO->OutputName.ToString());
				if (FO->A.Expression)
				{
					O->SetObjectField(TEXT("fed_by"), DescribeExpression(FO->A.Expression, 0, 4));
				}
				OutputsArr.Add(MakeShared<FJsonValueObject>(O));
			}
		}
		Info->SetArrayField(TEXT("function_inputs"), InputsArr);
		Info->SetArrayField(TEXT("function_outputs"), OutputsArr);

		// Duplicate node-id detection (same as materials).
		TSet<FGuid> SeenIds;
		TSet<FString> DupIds;
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Function->GetExpressionCollection().Expressions)
		{
			UMaterialExpression* E = ExprPtr;
			if (!E)
			{
				continue;
			}
			E->UpdateMaterialExpressionGuid(false, false);
			const FGuid Id = E->GetMaterialExpressionId();
			if (SeenIds.Contains(Id))
			{
				DupIds.Add(Id.ToString());
			}
			else
			{
				SeenIds.Add(Id);
			}
		}
		Info->SetBoolField(TEXT("has_duplicate_node_ids"), DupIds.Num() > 0);
		if (DupIds.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> DupArr;
			for (const FString& D : DupIds)
			{
				DupArr.Add(MakeShared<FJsonValueString>(D));
			}
			Info->SetArrayField(TEXT("duplicate_node_ids"), DupArr);
		}
		return Info;
	}

	// ── Topology-edit helpers (UMaterial OR UMaterialFunction) ───────────────────

	/** Load a material or material function for editing. Sets exactly one of the out pointers. */
	bool LoadEditableTarget(const FString& Path, UMaterial*& OutMaterial, UMaterialFunction*& OutFunction)
	{
		OutMaterial = nullptr;
		OutFunction = nullptr;
		UObject* Obj = LoadObject<UObject>(nullptr, *Path);
		if (!Obj)
		{
			return false;
		}
		OutMaterial = Cast<UMaterial>(Obj);
		OutFunction = Cast<UMaterialFunction>(Obj);
		return (OutMaterial != nullptr) || (OutFunction != nullptr);
	}

	FMaterialExpressionCollection* EditableCollection(UMaterial* M, UMaterialFunction* F)
	{
		if (M) { return &M->GetExpressionCollection(); }
		if (F) { return &F->GetExpressionCollection(); }
		return nullptr;
	}

	/** Find an expression by GUID. OutCount > 1 signals an ambiguous (duplicate) id. */
	UMaterialExpression* FindExprByGuid(UMaterial* M, UMaterialFunction* F, const FGuid& Id, int32& OutCount)
	{
		OutCount = 0;
		UMaterialExpression* Found = nullptr;
		FMaterialExpressionCollection* Coll = EditableCollection(M, F);
		if (!Coll)
		{
			return nullptr;
		}
		for (const TObjectPtr<UMaterialExpression>& ExprPtr : Coll->Expressions)
		{
			UMaterialExpression* E = ExprPtr;
			if (!E)
			{
				continue;
			}
			E->UpdateMaterialExpressionGuid(false, false);
			if (E->GetMaterialExpressionId() == Id)
			{
				if (!Found)
				{
					Found = E;
				}
				++OutCount;
			}
		}
		return Found;
	}

	/** Recompile + save after an edit. Functions recompile their dependent materials. */
	bool FinalizeEdit(UMaterial* M, UMaterialFunction* F)
	{
		UObject* Asset = nullptr;
		UPackage* Pkg = nullptr;
		if (M)
		{
			M->PostEditChange();
			M->MarkPackageDirty();
			Asset = M;
			Pkg = M->GetPackage();
		}
		else if (F)
		{
			UMaterialEditingLibrary::UpdateMaterialFunction(F, nullptr);
			F->MarkPackageDirty();
			Asset = F;
			Pkg = F->GetPackage();
		}
		if (!Asset || !Pkg)
		{
			return false;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::Save(Pkg, Asset, *File, Args).IsSuccessful();
	}

	/** Resolve a friendly expression class name ("Lerp", "Max", "Constant3Vector", "MaterialExpressionMax") to a UClass. */
	UClass* ResolveExpressionClass(const FString& InName)
	{
		FString Name = InName.TrimStartAndEnd();
		if (Name.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase))
		{
			Name = TEXT("LinearInterpolate");
		}
		TArray<FString> Candidates;
		if (Name.StartsWith(TEXT("MaterialExpression")))
		{
			Candidates.Add(Name);
		}
		else
		{
			Candidates.Add(FString(TEXT("MaterialExpression")) + Name);
		}
		Candidates.Add(Name);
		for (const FString& C : Candidates)
		{
			UClass* Cls = UClass::TryFindTypeSlow<UClass>(C);
			if (Cls && Cls->IsChildOf(UMaterialExpression::StaticClass()) && !Cls->HasAnyClassFlags(CLASS_Abstract))
			{
				return Cls;
			}
		}
		return nullptr;
	}

	/** Resolve "BaseColor" / "MP_BaseColor" / "EmissiveColor" etc. to an EMaterialProperty. */
	bool ResolveMaterialProperty(const FString& Name, EMaterialProperty& Out)
	{
		const UEnum* E = StaticEnum<EMaterialProperty>();
		if (!E)
		{
			return false;
		}
		for (int32 i = 0; i < E->NumEnums(); ++i)
		{
			const int64 Val = E->GetValueByIndex(i);
			FString Full = E->GetNameStringByValue(Val);
			FString Short = Full;
			int32 Underscore;
			if (Short.FindChar(TEXT('_'), Underscore))
			{
				Short = Short.RightChop(Underscore + 1);
			}
			if (Full.Equals(Name, ESearchCase::IgnoreCase) || Short.Equals(Name, ESearchCase::IgnoreCase))
			{
				Out = (EMaterialProperty)Val;
				return true;
			}
		}
		return false;
	}
#endif // WITH_EDITOR
}

TSharedPtr<FJsonObject> FMCPTool_Material::BuildMaterialInfoJson(UMaterialInterface* Material)
{
	TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();

	Info->SetStringField(TEXT("name"), Material->GetName());
	Info->SetStringField(TEXT("path"), Material->GetPathName());
	Info->SetStringField(TEXT("class"), Material->GetClass()->GetName());

	UMaterialInstance* MatInst = Cast<UMaterialInstance>(Material);
	Info->SetBoolField(TEXT("is_instance"), MatInst != nullptr);
	if (MatInst && MatInst->Parent)
	{
		Info->SetStringField(TEXT("parent"), MatInst->Parent->GetPathName());
	}

	// ── Rendering properties (virtual on UMaterialInterface — resolve for instances too) ──
	Info->SetStringField(TEXT("blend_mode"), EnumValueName(StaticEnum<EBlendMode>(), (int64)Material->GetBlendMode()));
	Info->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());

	const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
	const UEnum* ShadingEnum = StaticEnum<EMaterialShadingModel>();
	TArray<TSharedPtr<FJsonValue>> ShadingArr;
	for (int32 i = 0; i < MSM_NUM; ++i)
	{
		if (ShadingModels.HasShadingModel((EMaterialShadingModel)i))
		{
			ShadingArr.Add(MakeShared<FJsonValueString>(EnumValueName(ShadingEnum, i)));
		}
	}
	Info->SetArrayField(TEXT("shading_models"), ShadingArr);

	// ── Base-material properties (domain, usage flags, graph introspection) ──
	if (UMaterial* BaseMat = Material->GetMaterial())
	{
		Info->SetStringField(TEXT("material_domain"),
			EnumValueName(StaticEnum<EMaterialDomain>(), (int64)BaseMat->MaterialDomain.GetValue()));

		const UEnum* UsageEnum = StaticEnum<EMaterialUsage>();
		TArray<TSharedPtr<FJsonValue>> UsageArr;
		for (int32 i = 0; i < MATUSAGE_MAX; ++i)
		{
			if (BaseMat->GetUsageByFlag((EMaterialUsage)i))
			{
				UsageArr.Add(MakeShared<FJsonValueString>(EnumValueName(UsageEnum, i)));
			}
		}
		Info->SetArrayField(TEXT("usage_flags"), UsageArr);

#if WITH_EDITOR
		Info->SetBoolField(TEXT("use_material_attributes"), BaseMat->bUseMaterialAttributes);
		AddConnectedInputs(BaseMat, Info);
		AddCustomOutputs(BaseMat, Info);

		// Flag GUID collisions (distinct nodes sharing a node id → ambiguous for set_expression_value).
		{
			TSet<FGuid> SeenIds;
			TSet<FString> DupIds;
			for (const TObjectPtr<UMaterialExpression>& ExprPtr : BaseMat->GetExpressionCollection().Expressions)
			{
				UMaterialExpression* E = ExprPtr;
				if (!E)
				{
					continue;
				}
				E->UpdateMaterialExpressionGuid(false, false);
				const FGuid Id = E->GetMaterialExpressionId();
				if (SeenIds.Contains(Id))
				{
					DupIds.Add(Id.ToString());
				}
				else
				{
					SeenIds.Add(Id);
				}
			}
			Info->SetBoolField(TEXT("has_duplicate_node_ids"), DupIds.Num() > 0);
			if (DupIds.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> DupArr;
				for (const FString& D : DupIds)
				{
					DupArr.Add(MakeShared<FJsonValueString>(D));
				}
				Info->SetArrayField(TEXT("duplicate_node_ids"), DupArr);
			}
		}
#endif
	}

	// ── Parameters (defaults for base material, overrides for instance) ──
	AddMaterialParameters(Material, Info);

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
