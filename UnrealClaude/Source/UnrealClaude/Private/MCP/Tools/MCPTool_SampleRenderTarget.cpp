// Copyright Natali Caggiano. All Rights Reserved.

#include "MCPTool_SampleRenderTarget.h"
#include "MCP/MCPParamValidator.h"
#include "Claude_RenderTargetLibrary.h"
#include "Engine/TextureRenderTarget2DArray.h"

FMCPToolInfo FMCPTool_SampleRenderTarget::GetInfo() const
{
	FMCPToolInfo Info;
	Info.Name = TEXT("sample_render_target_2d_array");
	Info.Description = TEXT(
		"Sample a single texel from a TextureRenderTarget2DArray at the given UV and slice.\n\n"
		"Returns RGBA float values from the render target. Requires PIE to be running "
		"(or the Niagara system to have ticked at least once) so the RT contains data.\n\n"
		"iFFT ocean channel layout (RT_OceanWater_VertAttribs):\n"
		"  R = X displacement  G = Y displacement  B = Z displacement (height)  A = foam\n"
		"Cascade slice 3 = PatchLength 2000 (largest waves, use for buoyancy).\n"
		"UV formula for cascade 3:  u = frac(worldX / 2000),  v = frac(worldY / 2000)\n\n"
		"Example:\n"
		"  { \"rt_path\": \"/Game/OceanWater/RenderTargets/Vertex/RT_OceanWater_VertAttribs\","
		"    \"u\": 0.5, \"v\": 0.5, \"slice\": 3 }"
	);

	Info.Parameters.Add(FMCPToolParameter(TEXT("rt_path"), TEXT("string"),
		TEXT("Asset path of the TextureRenderTarget2DArray to sample"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("u"), TEXT("number"),
		TEXT("Horizontal UV coordinate [0..1]; wrapped via frac()"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("v"), TEXT("number"),
		TEXT("Vertical UV coordinate [0..1]; wrapped via frac()"), true));
	Info.Parameters.Add(FMCPToolParameter(TEXT("slice"), TEXT("number"),
		TEXT("Array slice index (0-based). iFFT: 0=10u, 1=28u, 2=432u, 3=2000u. Default: 3"), false));

	Info.Annotations = FMCPToolAnnotations::ReadOnly();
	return Info;
}

FMCPToolResult FMCPTool_SampleRenderTarget::Execute(const TSharedRef<FJsonObject>& Params)
{
	// --- Extract & validate parameters ---
	FString RTPath;
	TOptional<FMCPToolResult> Error;

	if (!ExtractRequiredString(Params, TEXT("rt_path"), RTPath, Error))
	{
		return Error.GetValue();
	}
	if (!ValidateBlueprintPathParam(RTPath, Error))
	{
		return Error.GetValue();
	}

	double U_In = 0.0, V_In = 0.0;
	if (!Params->TryGetNumberField(TEXT("u"), U_In))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: u"));
	}
	if (!Params->TryGetNumberField(TEXT("v"), V_In))
	{
		return FMCPToolResult::Error(TEXT("Missing required parameter: v"));
	}

	const int32 SliceIndex = ExtractOptionalNumber<int32>(Params, TEXT("slice"), 3);

	// --- Load the render target asset ---
	UTextureRenderTarget2DArray* RT = LoadObject<UTextureRenderTarget2DArray>(nullptr, *RTPath);
	if (!RT)
	{
		// Retry with explicit asset-name suffix (some paths omit it)
		RT = LoadObject<UTextureRenderTarget2DArray>(nullptr,
			*(RTPath + TEXT(".") + FPackageName::GetShortName(RTPath)));
	}
	if (!RT)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Failed to load TextureRenderTarget2DArray at '%s'. "
				 "Confirm the path points to a TextureRenderTarget2DArray asset."),
			*RTPath));
	}

	// --- Validate slice ---
	if (SliceIndex < 0 || SliceIndex >= RT->Slices)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Slice index %d out of range — RT '%s' has %d slice(s) (valid: 0..%d)."),
			SliceIndex, *RT->GetName(), RT->Slices, RT->Slices - 1));
	}

	// --- Sample ---
	const FLinearColor Sample = UClaude_RenderTargetLibrary::SampleRenderTarget2DArray(
		RT, static_cast<float>(U_In), static_cast<float>(V_In), SliceIndex);

	// Check for "not yet rendered" case — library returns Black when resource is null
	FTextureRenderTargetResource* Resource = RT->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return FMCPToolResult::Error(FString::Printf(
			TEXT("Render target '%s' has no GPU resource. "
				 "Start PIE (or tick the Niagara system) so the RT is populated before sampling."),
			*RT->GetName()));
	}

	// --- Build result ---
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("rt_path"), RTPath);
	ResultData->SetStringField(TEXT("rt_name"), RT->GetName());
	ResultData->SetNumberField(TEXT("width"), RT->SizeX);
	ResultData->SetNumberField(TEXT("height"), RT->SizeY);
	ResultData->SetNumberField(TEXT("total_slices"), RT->Slices);
	ResultData->SetNumberField(TEXT("slice_sampled"), SliceIndex);
	ResultData->SetNumberField(TEXT("u"), U_In);
	ResultData->SetNumberField(TEXT("v"), V_In);
	ResultData->SetNumberField(TEXT("r"), Sample.R);
	ResultData->SetNumberField(TEXT("g"), Sample.G);
	ResultData->SetNumberField(TEXT("b"), Sample.B);
	ResultData->SetNumberField(TEXT("a"), Sample.A);

	return FMCPToolResult::Success(
		FString::Printf(
			TEXT("RT2DArray '%s' [%dx%d, %d slices] slice %d UV(%.3f,%.3f) → "
				 "R=%.4f G=%.4f B=%.4f A=%.4f"),
			*RT->GetName(), RT->SizeX, RT->SizeY, RT->Slices, SliceIndex,
			static_cast<float>(U_In), static_cast<float>(V_In),
			Sample.R, Sample.G, Sample.B, Sample.A),
		ResultData);
}
