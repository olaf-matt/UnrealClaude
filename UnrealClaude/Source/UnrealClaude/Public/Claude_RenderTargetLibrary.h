// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Claude_RenderTargetLibrary.generated.h"

class UTextureRenderTarget2DArray;

/**
 * Blueprint function library for sampling TextureRenderTarget2DArray assets.
 *
 * The built-in ReadRenderTargetUV node only supports TextureRenderTarget2D.
 * Use SampleRenderTarget2DArray to sample a specific slice of a 2D array render
 * target from Blueprint at runtime — e.g. iFFT ocean displacement for buoyancy.
 *
 * Channel layout for RT_OceanWater_VertAttribs (iFFT vertex displacement):
 *   R = X displacement (horizontal, wave-forward)
 *   G = Y displacement (horizontal, perpendicular)
 *   B = Z displacement (vertical height) ← use for buoyancy
 *   A = Foam / Jacobian determinant
 *
 * Performance note: each call flushes rendering commands (one GPU sync per call).
 * For N pontoons, this means N syncs per frame. Acceptable for prototyping; for
 * production, prefer the Niagara async readback path.
 */
UCLASS()
class UNREALCLAUDE_API UClaude_RenderTargetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Sample a single texel from a specific slice of a TextureRenderTarget2DArray.
	 *
	 * UV coordinates are wrapped via frac() before sampling, so values outside [0,1]
	 * tile correctly. Returns FLinearColor::Black on failure (null RT, resource not
	 * ready, or slice out of range).
	 *
	 * For iFFT ocean buoyancy:
	 *   RenderTarget = RT_OceanWater_VertAttribs
	 *   U = frac(WorldX / OceanPatchLength)   [e.g. WorldX / 2000 for cascade 3]
	 *   V = frac(WorldY / OceanPatchLength)
	 *   SliceIndex = 3  (cascade 3, PatchLength=2000, largest swells)
	 *   Result.B = Z displacement in world units
	 *
	 * @param RenderTarget   The TextureRenderTarget2DArray asset to sample.
	 * @param U              Horizontal UV [0..1], wraps via frac.
	 * @param V              Vertical UV [0..1], wraps via frac.
	 * @param SliceIndex     Array slice (0-based). iFFT cascades: 0=tiny, 1=small, 2=medium, 3=large.
	 * @return               RGBA as linear float values. Black on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealClaude|RenderTarget",
		meta = (DisplayName = "Sample Render Target 2D Array",
			Keywords = "render target array slice ocean iFFT displacement buoyancy"))
	static FLinearColor SampleRenderTarget2DArray(
		UTextureRenderTarget2DArray* RenderTarget,
		float U,
		float V,
		int32 SliceIndex = 3);
};
