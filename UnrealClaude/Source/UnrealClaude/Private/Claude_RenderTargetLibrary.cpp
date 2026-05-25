// Copyright Natali Caggiano. All Rights Reserved.

#include "Claude_RenderTargetLibrary.h"

#include "Engine/TextureRenderTarget2DArray.h"
#include "RenderingThread.h"      // ENQUEUE_RENDER_COMMAND, FlushRenderingCommands
#include "RHI.h"                  // FRHITexture, FReadSurfaceDataFlags
#include "RHICommandList.h"       // FRHICommandListImmediate
#include "Math/Float16Color.h"    // FFloat16Color

FLinearColor UClaude_RenderTargetLibrary::SampleRenderTarget2DArray(
	UTextureRenderTarget2DArray* RenderTarget,
	float U,
	float V,
	int32 SliceIndex)
{
	if (!IsValid(RenderTarget))
	{
		return FLinearColor::Black;
	}

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return FLinearColor::Black;
	}

	// Clamp slice; wrap UV into [0,1)
	const int32 ClampedSlice = FMath::Clamp(SliceIndex, 0, RenderTarget->Slices - 1);
	const float WU = FMath::Frac(U < 0.0f ? U + FMath::CeilToFloat(-U) : U);
	const float WV = FMath::Frac(V < 0.0f ? V + FMath::CeilToFloat(-V) : V);
	const int32 TexelX = FMath::Clamp(FMath::FloorToInt(WU * RenderTarget->SizeX), 0, RenderTarget->SizeX - 1);
	const int32 TexelY = FMath::Clamp(FMath::FloorToInt(WV * RenderTarget->SizeY), 0, RenderTarget->SizeY - 1);

	// Pixel data is written on the render thread; use a thread-safe shared pointer
	// so that the lambda and game thread can both access it safely.
	TSharedPtr<TArray<FFloat16Color>, ESPMode::ThreadSafe> PixelData =
		MakeShared<TArray<FFloat16Color>, ESPMode::ThreadSafe>();

	ENQUEUE_RENDER_COMMAND(UClaude_SampleRT2DArray)(
		[Resource, TexelX, TexelY, ClampedSlice, PixelData](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* TextureRHI = Resource->GetRenderTargetTexture();
		if (!TextureRHI)
		{
			return;
		}

		// RCM_UNorm is the default; for ReadSurfaceFloatData the compression mode
		// is not applied — raw FFloat16 values are returned regardless.
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetArrayIndex(ClampedSlice);

		RHICmdList.ReadSurfaceFloatData(
			TextureRHI,
			FIntRect(TexelX, TexelY, TexelX + 1, TexelY + 1),
			*PixelData,
			ReadFlags);
	});

	// Block the game thread until the render command completes.
	// Cost: one GPU sync per call (~0.1-0.5 ms). Acceptable for ~4 pontoons/frame.
	FlushRenderingCommands();

	if (PixelData->Num() > 0)
	{
		const FFloat16Color& P = (*PixelData)[0];
		return FLinearColor(P.R.GetFloat(), P.G.GetFloat(), P.B.GetFloat(), P.A.GetFloat());
	}

	// Render target may not have been rendered yet (e.g., before first PIE tick).
	return FLinearColor::Black;
}
