// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"

/** Input parameter for AddFunction — name + parsed pin type. */
struct FBlueprintFunctionParam
{
	FString Name;
	FEdGraphPinType PinType;
};

/**
 * Blueprint variable and function management
 *
 * Responsibilities:
 * - Adding/removing member variables
 * - Adding/removing functions
 * - Type parsing and conversion
 * - Name validation
 *
 * Supported Types:
 * - Primitives: bool, int32, int64, float, double, byte, FString, FName, FText
 * - Structs: FVector, FRotator, FTransform, FLinearColor, FVector2D
 * - Containers: TArray<T>, TSet<T>
 * - Object references: "Actor*", "MaterialInstanceDynamic*", or bare name "MaterialInstanceDynamic"
 *   (any loaded C++ class — searches Engine, Niagara, UMG, and all loaded packages)
 */
class FBlueprintEditor
{
public:
	// ===== Variable Management =====

	/**
	 * Add member variable to Blueprint
	 * @param Blueprint - Blueprint to modify
	 * @param VariableName - Name of variable (must be valid identifier)
	 * @param PinType - Variable type
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool AddVariable(
		UBlueprint* Blueprint,
		const FString& VariableName,
		const FEdGraphPinType& PinType,
		FString& OutError
	);

	/**
	 * Remove variable from Blueprint
	 * @param Blueprint - Blueprint to modify
	 * @param VariableName - Name of variable to remove
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool RemoveVariable(
		UBlueprint* Blueprint,
		const FString& VariableName,
		FString& OutError
	);

	/**
	 * Set the default value of a Blueprint variable (primitive types).
	 * Accepts the same string format UE serializes internally:
	 *   float  → "25.0"
	 *   bool   → "true" or "false"
	 *   int    → "42"
	 *   string → "Hello"
	 *   Vector → "(X=0.0,Y=0.0,Z=0.0)"
	 * Object-reference variables cannot have meaningful defaults set this way.
	 */
	static bool SetVariableDefault(
		UBlueprint* Blueprint,
		const FString& VariableName,
		const FString& DefaultValue,
		FString& OutError
	);

	/**
	 * Set or clear the Expose on Spawn flag on a Blueprint variable.
	 * When true, the variable appears as a pin on SpawnActor/ConstructObject nodes.
	 */
	static bool SetVariableExposeOnSpawn(
		UBlueprint* Blueprint,
		const FString& VariableName,
		bool bExposeOnSpawn,
		FString& OutError
	);

	/**
	 * Rename a Blueprint variable, updating all graph references.
	 */
	static bool RenameVariable(
		UBlueprint* Blueprint,
		const FString& OldName,
		const FString& NewName,
		FString& OutError
	);

	/**
	 * Set or clear the Instance Editable flag on a Blueprint variable.
	 * When bInstanceEditable=true the variable appears in the Details panel for level instances
	 * and its value is serialized per-instance. When false (the default for new variables),
	 * set_property calls on level instances are silently discarded.
	 * @param Blueprint       - Blueprint to modify
	 * @param VariableName    - Name of the variable
	 * @param bInstanceEditable - true to enable, false to disable
	 * @param OutError        - Error message if failed
	 * @return true if successful
	 */
	static bool SetVariableInstanceEditable(
		UBlueprint* Blueprint,
		const FString& VariableName,
		bool bInstanceEditable,
		FString& OutError
	);

	// ===== Function Management =====

	/**
	 * Add function to Blueprint (no parameters).
	 */
	static bool AddFunction(
		UBlueprint* Blueprint,
		const FString& FunctionName,
		FString& OutError
	);

	/**
	 * Add function to Blueprint with typed input parameters.
	 * For Blueprint Interfaces this also marks the entry node as editable so the
	 * Details panel shows the signature editor (fixes the "Graph is not editable" bug).
	 */
	static bool AddFunction(
		UBlueprint* Blueprint,
		const FString& FunctionName,
		const TArray<FBlueprintFunctionParam>& InParams,
		FString& OutError
	);

	/**
	 * Add a single input pin to an existing function on a Blueprint Interface.
	 * For regular Blueprints, use add_function with the inputs array instead.
	 * @param Blueprint     - Blueprint to modify (must be BPTYPE_Interface)
	 * @param FunctionName  - Name of the function graph to modify
	 * @param InputName     - Name of the new input pin
	 * @param PinType       - Type of the new input pin
	 * @param OutError      - Error message if failed
	 * @return true if successful
	 */
	static bool AddFunctionInput(
		UBlueprint* Blueprint,
		const FString& FunctionName,
		const FString& InputName,
		const FEdGraphPinType& PinType,
		FString& OutError
	);

	/**
	 * Remove function from Blueprint
	 * @param Blueprint - Blueprint to modify
	 * @param FunctionName - Name of function to remove
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	static bool RemoveFunction(
		UBlueprint* Blueprint,
		const FString& FunctionName,
		FString& OutError
	);

	// ===== Type Conversion =====

	/**
	 * Parse type string to FEdGraphPinType
	 *
	 * Supported formats:
	 * - Primitives: "bool", "int32", "float", "FString"
	 * - Structs: "FVector", "FRotator", "FTransform"
	 * - Arrays: "TArray<int32>", "TArray<FVector>", "TArray<MaterialInstanceDynamic>"
	 * - Objects: "Actor*", "MaterialInstanceDynamic*", or bare "MaterialInstanceDynamic"
	 *   (searches Engine, Niagara, UMG, and all loaded packages)
	 *
	 * @param TypeString - Type name string
	 * @param OutPinType - Output pin type
	 * @param OutError - Error message if parsing fails
	 * @return true if successful
	 */
	static bool ParsePinType(
		const FString& TypeString,
		FEdGraphPinType& OutPinType,
		FString& OutError
	);

	/**
	 * Convert FEdGraphPinType to string representation
	 * @param PinType - Pin type to convert
	 * @return Type string
	 */
	static FString PinTypeToString(const FEdGraphPinType& PinType);

	// ===== Name Validation =====

	/**
	 * Validate variable name follows Blueprint naming conventions
	 * Rules: max 128 chars, starts with letter/underscore, alphanumeric + underscore only
	 * @param VariableName - Name to validate
	 * @param OutError - Error message if invalid
	 * @return true if valid
	 */
	static bool ValidateVariableName(const FString& VariableName, FString& OutError);

	/**
	 * Validate function name follows Blueprint naming conventions
	 * (Same rules as variable names)
	 * @param FunctionName - Name to validate
	 * @param OutError - Error message if invalid
	 * @return true if valid
	 */
	static bool ValidateFunctionName(const FString& FunctionName, FString& OutError);

	// ===== Component Management =====

	/**
	 * Add a component to a Blueprint's SimpleConstructionScript.
	 * @param ComponentClassName  C++ class name without U prefix: "NiagaraComponent", "StaticMeshComponent", etc.
	 * @param ComponentName       Variable name for the component (e.g. "WaterNiagara", "DeckMesh")
	 * @param AssetPath           Optional asset path to assign (e.g. "/Game/Blueprints/ShallowWater/FX_ShallowWater")
	 */
	static bool AddComponent(
		UBlueprint* Blueprint,
		const FString& ComponentClassName,
		const FString& ComponentName,
		const FString& AssetPath,
		FString& OutError
	);

	/**
	 * Remove a component from a Blueprint's SimpleConstructionScript by variable name.
	 */
	static bool RemoveComponent(
		UBlueprint* Blueprint,
		const FString& ComponentName,
		FString& OutError
	);

	/**
	 * Set a property on a component template in a Blueprint's SimpleConstructionScript.
	 * For object reference properties (mesh, asset), pass the asset path.
	 * For primitive properties, pass the value as string (same format as UE serializes).
	 */
	static bool SetComponentProperty(
		UBlueprint* Blueprint,
		const FString& ComponentName,
		const FString& PropertyName,
		const FString& Value,
		FString& OutError
	);

	// ===== Blueprint Class Management =====

	/**
	 * Make a Blueprint implement a Blueprint Interface.
	 * Creates empty function stubs for all interface functions.
	 * @param InterfaceName  Short name of the interface, e.g. "BPI_WindReceiver" (with or without _C suffix)
	 */
	static bool AddInterface(
		UBlueprint* Blueprint,
		const FString& InterfaceName,
		FString& OutError
	);

private:
	// Constants
	static constexpr int32 MaxNameLength = 128;

	// Helper for parsing container types
	static bool ParseContainerType(
		const FString& TypeString,
		FEdGraphPinType& OutPinType,
		FString& OutError
	);

	// Helper for parsing struct types
	static bool ParseStructType(
		const FString& TypeName,
		FEdGraphPinType& OutPinType,
		FString& OutError
	);
};
