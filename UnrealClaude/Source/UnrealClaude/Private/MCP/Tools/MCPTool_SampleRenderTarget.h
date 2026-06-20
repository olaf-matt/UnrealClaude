// Copyright Natali Caggiano. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/MCPToolBase.h"
#include "Dom/JsonObject.h"

/**
 * MCP Tool: sample a single texel from a TextureRenderTarget2DArray.
 *
 * Wraps UClaude_RenderTargetLibrary::SampleRenderTarget2DArray for use by the
 * editor assistant.  Useful for verifying that the iFFT ocean RT contains valid
 * displacement data before wiring up the buoyancy Blueprint.
 *
 * Parameters:
 *   rt_path  (string, required) — asset path to the TextureRenderTarget2DArray
 *   u        (number, required) — UV horizontal [0..1]
 *   v        (number, required) — UV vertical   [0..1]
 *   slice    (number, optional, default 3) — array slice index
 *
 * Returns RGBA float values plus RT dimensions.
 * Requires PIE to be running (or Niagara to have ticked) so the RT has content.
 */
class FMCPTool_SampleRenderTarget : public FMCPToolBase
{
public:
	virtual FMCPToolInfo GetInfo() const override;
	virtual FMCPToolResult Execute(const TSharedRef<FJsonObject>& Params) override;
};
