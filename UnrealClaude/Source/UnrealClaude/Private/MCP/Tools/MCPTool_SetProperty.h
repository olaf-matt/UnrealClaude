// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"

/**
 * MCP Tool: Set a property on an actor
 */
class FMCPTool_SetProperty : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override
	{
		FMCPToolInfo Info;
		Info.Name = TEXT("set_property");
		Info.Description = TEXT(
			"Set any property value on an actor, including component sub-properties.\n\n"
			"Use dot notation to access components and nested properties.\n\n"
			"PROPERTY PATH EXAMPLES:\n"
			"  'bHidden'                              Actor visibility (bool)\n"
			"  'LightComponent.Intensity'             Light intensity (number)\n"
			"  'LightComponent.LightColor'            Light color (object or hex)\n"
			"  'LightComponent.AttenuationRadius'     Light radius (number)\n"
			"  'StaticMeshComponent.RelativeScale3D'  Mesh scale (object)\n"
			"  'StaticMeshComponent.StaticMesh'       Mesh asset (string path)\n"
			"  'RootComponent.RelativeLocation'       Root position (object)\n\n"
			"VALUE FORMATS:\n"
			"  number:         42  or  3.14\n"
			"  bool:           true  or  false\n"
			"  string:         \"hello\"\n"
			"  asset ref:      \"/Game/Meshes/SM_Rock\"\n"
			"  FVector:        {\"X\":100,\"Y\":0,\"Z\":50}   (uppercase X/Y/Z)\n"
			"  FRotator:       {\"Pitch\":0,\"Yaw\":90,\"Roll\":0}  (uppercase)\n"
			"  FLinearColor:   {\"R\":1,\"G\":0.5,\"B\":0,\"A\":1}  or hex \"#FF8800FF\"\n\n"
			"Component names are capitalized exactly as in the class (e.g., 'LightComponent',\n"
			"'StaticMeshComponent', 'ExponentialHeightFogComponent').\n"
			"Returns: Confirmation of property change."
		);
		Info.Parameters = {
			FMCPToolParameter(TEXT("actor_name"), TEXT("string"), TEXT("Actor Outliner label or internal name. Labels are set via 'name' in spawn_actor. Use get_level_actors to find exact names."), true),
			FMCPToolParameter(TEXT("property"), TEXT("string"), TEXT("Property path using dot notation (e.g., 'bHidden', 'LightComponent.Intensity', 'RootComponent.RelativeLocation')"), true),
			FMCPToolParameter(TEXT("value"), TEXT("any"), TEXT("Value to set. Type must match the property: number, bool, string, asset path, or struct object {X,Y,Z} / {R,G,B,A} / hex string."), true)
		};
		Info.Annotations = FMCPToolAnnotations::Modifying();
		return Info;
	}

	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;

private:
	/** Navigate through a property path to find the target object and property */
	bool NavigateToProperty(
		UObject* StartObject,
		const TArray<FString>& PathParts,
		UObject*& OutObject,
		FProperty*& OutProperty,
		FString& OutError);

	/** Try to navigate into a component on an actor */
	bool TryNavigateToComponent(
		UObject*& CurrentObject,
		const FString& PartName,
		bool bIsLastPart,
		FString& OutError);

	/** Navigate into a nested object property */
	bool NavigateIntoNestedObject(
		UObject*& CurrentObject,
		FProperty* Property,
		const FString& PartName,
		FString& OutError);

	/** Set a numeric property value from JSON */
	bool SetNumericPropertyValue(FNumericProperty* NumProp, void* ValuePtr, const TSharedPtr<FJsonValue>& Value);

	/** Set a struct property value from JSON (FVector, FRotator, FLinearColor) */
	bool SetStructPropertyValue(FStructProperty* StructProp, void* ValuePtr, const TSharedPtr<FJsonValue>& Value);

	/** Set an object reference property from a string asset path */
	bool SetObjectPropertyValue(FObjectProperty* ObjProp, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/** Helper to set a property value from JSON */
	bool SetPropertyFromJson(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, FString& OutError);
};
