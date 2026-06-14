// Game_render.cpp
//


#include "Game_local.h"
#include "../renderer/ImageOpts.h"

idCVar g_renderCasUpscale("g_renderCasUpscale", "0", CVAR_BOOL, "toggles the optional CAS post-process pass when a CAS material is available");
idCVar g_renderFastNoPost("g_renderFastNoPost", "1", CVAR_BOOL, "render through the direct no-post path when AA, blur, and CAS are disabled");
idCVar g_renderFastNoPostDirect("g_renderFastNoPostDirect", "1", CVAR_BOOL, "render directly to the backbuffer when the no-post path has no active post-processing work");
idCVar g_renderCaptureCurrentRender("g_renderCaptureCurrentRender", "0", CVAR_BOOL, "force an end-of-view _currentRender copy instead of relying on on-demand post-process capture");

enum openq4PostAAMode_t {
	OPENQ4_POST_AA_OFF = 0,
	OPENQ4_POST_AA_SMAA_1X_MEDIUM,
	OPENQ4_POST_AA_SMAA_1X_HIGH,
	OPENQ4_POST_AA_SMAA_1X_ULTRA,
	OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE
};

enum openq4PostAASourceColorSpace_t {
	OPENQ4_POST_AA_SOURCE_NONE = -1,
	OPENQ4_POST_AA_SOURCE_LEGACY_PERCEPTUAL_LDR = 0,
	OPENQ4_POST_AA_SOURCE_LINEAR_LDR
};

enum openq4PostAADisableReason_t {
	OPENQ4_POST_AA_DISABLE_NONE = 0,
	OPENQ4_POST_AA_DISABLE_UNSUPPORTED_MODE,
	OPENQ4_POST_AA_DISABLE_UNAVAILABLE,
	OPENQ4_POST_AA_DISABLE_POST_LIGHTING_STACK,
	OPENQ4_POST_AA_DISABLE_INVALID_TARGET_SIZE
};

enum openq4PostAAWarningState_t {
	OPENQ4_POST_AA_WARNING_NONE = 0,
	OPENQ4_POST_AA_WARNING_UNSUPPORTED_MODE,
	OPENQ4_POST_AA_WARNING_UNAVAILABLE,
	OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK,
	OPENQ4_POST_AA_WARNING_INVALID_TARGET_SIZE
};

struct openq4PostAASchedule_t {
	int requestedCvarValue;
	openq4PostAAMode_t requestedMode;
	openq4PostAAMode_t effectiveMode;
	openq4PostAADisableReason_t disableReason;
	idRenderTexture* resolveTarget;
	const char* sourceImageName;
	const char* outputImageName;
	int sourceWidth;
	int sourceHeight;
	int outputWidth;
	int outputHeight;
	openq4PostAASourceColorSpace_t sourceColorSpace;
	const char* sourceColorSpaceName;
	const char* smaaQualityName;
	idVec4 smaaQuality;
};

struct openq4SMAAResources_t {
	idRenderTexture* sceneSource;
	idRenderTexture* edgeBlend;
	idRenderTexture* weightsFinal;
	const char* sceneSourceName;
	const char* edgeBlendName;
	const char* weightsFinalName;
};

struct openq4SMAAPass_t {
	const char* passName;
	const idMaterial* material;
	idRenderTexture* primarySource;
	const char* primarySourceName;
	idRenderTexture* secondarySource;
	const char* secondarySourceName;
	idRenderTexture* output;
	const char* outputName;
	bool clearBeforeDraw;
	bool bindPostProcessSourceSize;
	bool bindPostProcessSourceColorSpace;
	openq4PostAASourceColorSpace_t sourceColorSpace;
};

static bool IsUsablePostProcessMaterial( const idMaterial* material ) {
	return material != NULL && !material->TestMaterialFlag( MF_DEFAULTED );
}

static openq4PostAASourceColorSpace_t ResolvePostAASourceColorSpace( void ) {
	return OPENQ4_POST_AA_SOURCE_LEGACY_PERCEPTUAL_LDR;
}

static const char* PostAASourceColorSpaceName( const openq4PostAASourceColorSpace_t sourceColorSpace ) {
	switch ( sourceColorSpace ) {
	case OPENQ4_POST_AA_SOURCE_LINEAR_LDR:
		return "linear-ldr";
	case OPENQ4_POST_AA_SOURCE_LEGACY_PERCEPTUAL_LDR:
		return "legacy-perceptual-ldr";
	default:
		return "none";
	}
}

static idVec4 PostAASourceColorSpaceVector( const openq4PostAASourceColorSpace_t sourceColorSpace ) {
	switch ( sourceColorSpace ) {
	case OPENQ4_POST_AA_SOURCE_LINEAR_LDR:
		return idVec4( 1.0f, 2.2f, 0.0f, 0.0f );
	default:
		return idVec4( 0.0f, 2.2f, 0.0f, 0.0f );
	}
}

static bool PostAAModeUsesSMAA( const openq4PostAAMode_t mode ) {
	return mode == OPENQ4_POST_AA_SMAA_1X_MEDIUM ||
		mode == OPENQ4_POST_AA_SMAA_1X_HIGH ||
		mode == OPENQ4_POST_AA_SMAA_1X_ULTRA ||
		mode == OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE;
}

static const char* PostAASMAAQualityName( const openq4PostAAMode_t mode ) {
	switch ( mode ) {
	case OPENQ4_POST_AA_SMAA_1X_HIGH:
		return "high-luma";
	case OPENQ4_POST_AA_SMAA_1X_ULTRA:
		return "ultra-luma";
	case OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE:
		return "color-edge-prototype";
	case OPENQ4_POST_AA_SMAA_1X_MEDIUM:
		return "medium-luma";
	default:
		return "none";
	}
}

static idVec4 PostAASMAAQualityVector( const openq4PostAAMode_t mode ) {
	switch ( mode ) {
	case OPENQ4_POST_AA_SMAA_1X_HIGH:
		return idVec4( 0.0f, 0.10f, 16.0f, 2.0f );
	case OPENQ4_POST_AA_SMAA_1X_ULTRA:
		return idVec4( 0.0f, 0.05f, 32.0f, 2.0f );
	case OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE:
		return idVec4( 1.0f, 0.10f, 16.0f, 2.0f );
	case OPENQ4_POST_AA_SMAA_1X_MEDIUM:
	default:
		return idVec4( 0.0f, 0.10f, 8.0f, 2.0f );
	}
}

static openq4SMAAResources_t BuildSMAAResources( rvmGameRender_t& gameRender ) {
	openq4SMAAResources_t resources;
	resources.sceneSource = gameRender.postProcessRT[2];
	resources.edgeBlend = gameRender.postProcessRT[1];
	resources.weightsFinal = gameRender.postProcessRT[0];
	resources.sceneSourceName = "_postProcessAlbedo2/source";
	resources.edgeBlendName = "_postProcessAlbedo1/edge-blend";
	resources.weightsFinalName = "_postProcessAlbedo0/weights-final";
	return resources;
}

static bool ValidateDistinctPostAAResource(
	const idRenderTexture* a,
	const char* aName,
	const idRenderTexture* b,
	const char* bName,
	const char* context ) {
	assert( a != NULL && b != NULL );
	if ( a == NULL || b == NULL ) {
		return false;
	}

	const bool distinct = ( a != b );
	assert( distinct );
	if ( !distinct ) {
		common->Warning(
			"PostAA SMAA %s aliases %s and %s; disabling SMAA to avoid render-target feedback.",
			context,
			aName,
			bName );
		return false;
	}

	return true;
}

static bool ValidateSMAAResourceOwnership( const openq4SMAAResources_t& resources ) {
	return ValidateDistinctPostAAResource(
			resources.sceneSource,
			resources.sceneSourceName,
			resources.edgeBlend,
			resources.edgeBlendName,
			"resource contract" ) &&
		ValidateDistinctPostAAResource(
			resources.sceneSource,
			resources.sceneSourceName,
			resources.weightsFinal,
			resources.weightsFinalName,
			"resource contract" ) &&
		ValidateDistinctPostAAResource(
			resources.edgeBlend,
			resources.edgeBlendName,
			resources.weightsFinal,
			resources.weightsFinalName,
			"resource contract" );
}

static bool QueryRenderTextureSize( idRenderTexture* renderTexture, int& width, int& height ) {
	width = 0;
	height = 0;

	if ( renderTexture == NULL ) {
		return false;
	}

	renderSystem->GetRenderTextureSize( renderTexture, width, height );
	return width > 0 && height > 0;
}

static void SetPostAAScheduleSize( openq4PostAASchedule_t& schedule, idRenderTexture* sourceRT, idRenderTexture* outputRT ) {
	QueryRenderTextureSize( sourceRT, schedule.sourceWidth, schedule.sourceHeight );
	QueryRenderTextureSize( outputRT, schedule.outputWidth, schedule.outputHeight );
}

static bool ValidateSMAARenderTargetSizes( rvmGameRender_t& gameRender ) {
	const openq4SMAAResources_t resources = BuildSMAAResources( gameRender );
	if ( !ValidateSMAAResourceOwnership( resources ) ) {
		return false;
	}

	int width0, height0;
	int width1, height1;
	int width2, height2;
	const bool hasRT0 = QueryRenderTextureSize( gameRender.postProcessRT[0], width0, height0 );
	const bool hasRT1 = QueryRenderTextureSize( gameRender.postProcessRT[1], width1, height1 );
	const bool hasRT2 = QueryRenderTextureSize( gameRender.postProcessRT[2], width2, height2 );

	assert( hasRT0 && hasRT1 && hasRT2 );
	if ( !hasRT0 || !hasRT1 || !hasRT2 ) {
		return false;
	}

	const bool matchingSizes =
		width0 == width1 && width0 == width2 &&
		height0 == height1 && height0 == height2;
	assert( matchingSizes );
	return matchingSizes;
}

static bool SetSMAAPassSourceSize( const char* passName, idRenderTexture* sourceRT, idRenderTexture* outputRT ) {
	int sourceWidth, sourceHeight;
	int outputWidth, outputHeight;
	const bool hasSource = QueryRenderTextureSize( sourceRT, sourceWidth, sourceHeight );
	const bool hasOutput = QueryRenderTextureSize( outputRT, outputWidth, outputHeight );

	assert( hasSource && hasOutput );
	if ( !hasSource || !hasOutput ) {
		common->Warning( "PostAA SMAA %s pass has an invalid source or output render target; skipping SMAA.", passName );
		return false;
	}

	const bool matchingSizes = sourceWidth == outputWidth && sourceHeight == outputHeight;
	assert( matchingSizes );
	if ( !matchingSizes ) {
		common->Warning(
			"PostAA SMAA %s pass source/output size mismatch: source=%dx%d output=%dx%d; skipping SMAA.",
			passName,
			sourceWidth,
			sourceHeight,
			outputWidth,
			outputHeight );
		return false;
	}

	renderSystem->SetPostProcessSourceSize( sourceWidth, sourceHeight );
	return true;
}

static bool ValidateSMAAPassAliasing( const openq4SMAAPass_t& pass ) {
	assert( pass.output != NULL );
	assert( pass.primarySource != NULL );
	assert( pass.clearBeforeDraw );

	if ( pass.output == NULL || pass.primarySource == NULL ) {
		common->Warning( "PostAA SMAA %s pass has an incomplete resource contract; skipping SMAA.", pass.passName );
		return false;
	}

	if ( !pass.clearBeforeDraw ) {
		common->Warning( "PostAA SMAA %s pass is missing its mandatory pre-draw clear; skipping SMAA.", pass.passName );
		return false;
	}

	if ( !ValidateDistinctPostAAResource(
			pass.primarySource,
			pass.primarySourceName,
			pass.output,
			pass.outputName,
			pass.passName ) ) {
		return false;
	}

	if ( pass.secondarySource != NULL &&
		!ValidateDistinctPostAAResource(
			pass.secondarySource,
			pass.secondarySourceName,
			pass.output,
			pass.outputName,
			pass.passName ) ) {
		return false;
	}

	return true;
}

static bool ValidateSMAAPassSize( const openq4SMAAPass_t& pass ) {
	int sourceWidth, sourceHeight;
	int outputWidth, outputHeight;
	const bool hasSource = QueryRenderTextureSize( pass.primarySource, sourceWidth, sourceHeight );
	const bool hasOutput = QueryRenderTextureSize( pass.output, outputWidth, outputHeight );

	assert( hasSource && hasOutput );
	if ( !hasSource || !hasOutput ) {
		common->Warning( "PostAA SMAA %s pass has an invalid source or output render target; skipping SMAA.", pass.passName );
		return false;
	}

	const bool matchingSizes = sourceWidth == outputWidth && sourceHeight == outputHeight;
	assert( matchingSizes );
	if ( !matchingSizes ) {
		common->Warning(
			"PostAA SMAA %s pass source/output size mismatch: source=%dx%d output=%dx%d; skipping SMAA.",
			pass.passName,
			sourceWidth,
			sourceHeight,
			outputWidth,
			outputHeight );
		return false;
	}

	return true;
}

static void LogSMAARenderTargetSizes( rvmGameRender_t& gameRender ) {
	int width0, height0;
	int width1, height1;
	int width2, height2;
	if ( !QueryRenderTextureSize( gameRender.postProcessRT[0], width0, height0 ) ||
		!QueryRenderTextureSize( gameRender.postProcessRT[1], width1, height1 ) ||
		!QueryRenderTextureSize( gameRender.postProcessRT[2], width2, height2 ) ) {
		return;
	}

	static int loggedWidth0 = -1;
	static int loggedHeight0 = -1;
	static int loggedWidth1 = -1;
	static int loggedHeight1 = -1;
	static int loggedWidth2 = -1;
	static int loggedHeight2 = -1;
	if ( loggedWidth0 == width0 && loggedHeight0 == height0 &&
		loggedWidth1 == width1 && loggedHeight1 == height1 &&
		loggedWidth2 == width2 && loggedHeight2 == height2 ) {
		return;
	}

	common->Printf(
		"PostAA SMAA target sizes: _postProcessAlbedo2/source=%dx%d, _postProcessAlbedo1/edge=%dx%d, _postProcessAlbedo0/weights=%dx%d\n",
		width2,
		height2,
		width1,
		height1,
		width0,
		height0 );

	loggedWidth0 = width0;
	loggedHeight0 = height0;
	loggedWidth1 = width1;
	loggedHeight1 = height1;
	loggedWidth2 = width2;
	loggedHeight2 = height2;
}

static void LogSMAAResourceOwnership( const openq4SMAAResources_t& resources, const openq4PostAASourceColorSpace_t sourceColorSpace, const openq4PostAAMode_t mode ) {
	static idRenderTexture* loggedSceneSource = NULL;
	static idRenderTexture* loggedEdgeBlend = NULL;
	static idRenderTexture* loggedWeightsFinal = NULL;
	static int loggedSourceColorSpace = -1;
	static int loggedMode = -1;
	if ( loggedSceneSource == resources.sceneSource &&
		loggedEdgeBlend == resources.edgeBlend &&
		loggedWeightsFinal == resources.weightsFinal &&
		loggedSourceColorSpace == sourceColorSpace &&
		loggedMode == mode ) {
		return;
	}

	common->Printf(
		"PostAA SMAA resource ownership: sceneSource=%s, edgeBlend=%s, weightsFinal=%s; sourceSpace=%s; quality=%s; pass clears=edge,weights,blend,normalize\n",
		resources.sceneSourceName,
		resources.edgeBlendName,
		resources.weightsFinalName,
		PostAASourceColorSpaceName( sourceColorSpace ),
		PostAASMAAQualityName( mode ) );

	loggedSceneSource = resources.sceneSource;
	loggedEdgeBlend = resources.edgeBlend;
	loggedWeightsFinal = resources.weightsFinal;
	loggedSourceColorSpace = sourceColorSpace;
	loggedMode = mode;
}

static const char* PostAAModeName( const openq4PostAAMode_t mode ) {
	switch ( mode ) {
	case OPENQ4_POST_AA_SMAA_1X_MEDIUM:
		return "SMAA1xMedium";
	case OPENQ4_POST_AA_SMAA_1X_HIGH:
		return "SMAA1xHigh";
	case OPENQ4_POST_AA_SMAA_1X_ULTRA:
		return "SMAA1xUltra";
	case OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE:
		return "SMAA1xColorPrototype";
	default:
		return "Off";
	}
}

static const char* PostAADisableReasonName( const openq4PostAADisableReason_t reason ) {
	switch ( reason ) {
	case OPENQ4_POST_AA_DISABLE_UNSUPPORTED_MODE:
		return "unsupported-mode";
	case OPENQ4_POST_AA_DISABLE_UNAVAILABLE:
		return "resources-unavailable";
	case OPENQ4_POST_AA_DISABLE_POST_LIGHTING_STACK:
		return "post-lighting-stack";
	case OPENQ4_POST_AA_DISABLE_INVALID_TARGET_SIZE:
		return "invalid-target-size";
	default:
		return "none";
	}
}

static openq4PostAAMode_t TranslatePostAASettings( const int cvarValue ) {
	switch ( cvarValue ) {
	case 1:
		return OPENQ4_POST_AA_SMAA_1X_MEDIUM;
	case 2:
		return OPENQ4_POST_AA_SMAA_1X_HIGH;
	case 3:
		return OPENQ4_POST_AA_SMAA_1X_ULTRA;
	case 4:
		return OPENQ4_POST_AA_SMAA_1X_COLOR_PROTOTYPE;
	default:
		return OPENQ4_POST_AA_OFF;
	}
}

static bool PostAAModeSupported( const int cvarValue ) {
	return cvarValue >= 0 && cvarValue <= 4;
}

static bool ValidateSMAAMaterial( const idMaterial* material, const char* materialName ) {
	if ( !IsUsablePostProcessMaterial( material ) ) {
		return false;
	}

	if ( material->GetNumStages() <= 0 ) {
		common->Warning( "SMAA material '%s' has no stages; disabling SMAA.", materialName );
		return false;
	}

	if ( !renderSystem->ValidateMaterialArbPrograms( material ) ) {
		common->Warning( "SMAA material '%s' uses an invalid or unavailable shader program; disabling SMAA.", materialName );
		return false;
	}

	return true;
}

static bool EvaluateSMAAAvailability( rvmGameRender_t& gameRender ) {
	return ValidateSMAAMaterial(
		gameRender.smaaEdgePostProcessMaterial,
		( gameRender.smaaEdgePostProcessMaterial != NULL ) ? gameRender.smaaEdgePostProcessMaterial->GetName() : "postprocess/smaa_edge" ) &&
		ValidateSMAAMaterial(
			gameRender.smaaWeightsPostProcessMaterial,
			( gameRender.smaaWeightsPostProcessMaterial != NULL ) ? gameRender.smaaWeightsPostProcessMaterial->GetName() : "postprocess/smaa_weights" ) &&
		ValidateSMAAMaterial(
			gameRender.smaaBlendPostProcessMaterial,
			( gameRender.smaaBlendPostProcessMaterial != NULL ) ? gameRender.smaaBlendPostProcessMaterial->GetName() : "postprocess/smaa_blend" ) &&
		renderSystem->ValidateSMAALookupTextures() &&
		( gameRender.postProcessRT[0] != NULL ) &&
		( gameRender.postProcessRT[1] != NULL ) &&
		( gameRender.postProcessRT[2] != NULL ) &&
		ValidateSMAAResourceOwnership( BuildSMAAResources( gameRender ) ) &&
		IsUsablePostProcessMaterial( gameRender.copyPostProcess1Material );
}

static openq4PostAASchedule_t BuildPostAAPasses( rvmGameRender_t& gameRender, const bool postLightingStackEnabled ) {
	openq4PostAASchedule_t schedule;
	schedule.requestedCvarValue = cvarSystem->GetCVarInteger( "r_postAA" );
	schedule.requestedMode = TranslatePostAASettings( schedule.requestedCvarValue );
	schedule.effectiveMode = schedule.requestedMode;
	schedule.disableReason = OPENQ4_POST_AA_DISABLE_NONE;
	schedule.resolveTarget = gameRender.postProcessRT[0];
	schedule.sourceImageName = "_postProcessAlbedo0";
	schedule.outputImageName = "_postProcessAlbedo0";
	schedule.sourceWidth = 0;
	schedule.sourceHeight = 0;
	schedule.outputWidth = 0;
	schedule.outputHeight = 0;
	schedule.sourceColorSpace = OPENQ4_POST_AA_SOURCE_NONE;
	schedule.sourceColorSpaceName = PostAASourceColorSpaceName( schedule.sourceColorSpace );
	schedule.smaaQualityName = PostAASMAAQualityName( schedule.requestedMode );
	schedule.smaaQuality = PostAASMAAQualityVector( schedule.requestedMode );
	SetPostAAScheduleSize( schedule, gameRender.postProcessRT[0], gameRender.postProcessRT[0] );

	if ( !PostAAModeSupported( schedule.requestedCvarValue ) ) {
		schedule.effectiveMode = OPENQ4_POST_AA_OFF;
		schedule.disableReason = OPENQ4_POST_AA_DISABLE_UNSUPPORTED_MODE;
		return schedule;
	}

	if ( PostAAModeUsesSMAA( schedule.requestedMode ) ) {
		if ( postLightingStackEnabled ) {
			schedule.effectiveMode = OPENQ4_POST_AA_OFF;
			schedule.disableReason = OPENQ4_POST_AA_DISABLE_POST_LIGHTING_STACK;
		} else if ( !gameRender.smaaAvailable ) {
			schedule.effectiveMode = OPENQ4_POST_AA_OFF;
			schedule.disableReason = OPENQ4_POST_AA_DISABLE_UNAVAILABLE;
		} else if ( !ValidateSMAARenderTargetSizes( gameRender ) ) {
			schedule.effectiveMode = OPENQ4_POST_AA_OFF;
			schedule.disableReason = OPENQ4_POST_AA_DISABLE_INVALID_TARGET_SIZE;
		} else {
			schedule.resolveTarget = gameRender.postProcessRT[2];
			schedule.sourceImageName = "_postProcessAlbedo2";
			schedule.outputImageName = "_postProcessAlbedo0";
			schedule.sourceColorSpace = ResolvePostAASourceColorSpace();
			schedule.sourceColorSpaceName = PostAASourceColorSpaceName( schedule.sourceColorSpace );
			schedule.smaaQualityName = PostAASMAAQualityName( schedule.effectiveMode );
			schedule.smaaQuality = PostAASMAAQualityVector( schedule.effectiveMode );
			SetPostAAScheduleSize( schedule, gameRender.postProcessRT[2], gameRender.postProcessRT[0] );
		}
	}

	return schedule;
}

static int PostAAWarningStateForSchedule( const openq4PostAASchedule_t& schedule ) {
	switch ( schedule.disableReason ) {
	case OPENQ4_POST_AA_DISABLE_UNSUPPORTED_MODE:
		return OPENQ4_POST_AA_WARNING_UNSUPPORTED_MODE;
	case OPENQ4_POST_AA_DISABLE_UNAVAILABLE:
		return OPENQ4_POST_AA_WARNING_UNAVAILABLE;
	case OPENQ4_POST_AA_DISABLE_POST_LIGHTING_STACK:
		return OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK;
	case OPENQ4_POST_AA_DISABLE_INVALID_TARGET_SIZE:
		return OPENQ4_POST_AA_WARNING_INVALID_TARGET_SIZE;
	default:
		return OPENQ4_POST_AA_WARNING_NONE;
	}
}

static void LogPostAASchedule( const openq4PostAASchedule_t& schedule, const bool smaaResourcesAvailable ) {
	static int loggedRequestedValue = -9999;
	static int loggedRequestedMode = -1;
	static int loggedEffectiveMode = -1;
	static int loggedDisableReason = -1;
	static int loggedSMAAResourcesAvailable = -1;
	static int loggedSourceWidth = -1;
	static int loggedSourceHeight = -1;
	static int loggedOutputWidth = -1;
	static int loggedOutputHeight = -1;
	static int loggedSourceColorSpace = -1;
	static idStr loggedSMAAQualityName;
	const int smaaResourcesFlag = smaaResourcesAvailable ? 1 : 0;

	if ( loggedRequestedValue == schedule.requestedCvarValue &&
		loggedRequestedMode == schedule.requestedMode &&
		loggedEffectiveMode == schedule.effectiveMode &&
		loggedDisableReason == schedule.disableReason &&
		loggedSMAAResourcesAvailable == smaaResourcesFlag &&
		loggedSourceWidth == schedule.sourceWidth &&
		loggedSourceHeight == schedule.sourceHeight &&
		loggedOutputWidth == schedule.outputWidth &&
		loggedOutputHeight == schedule.outputHeight &&
		loggedSourceColorSpace == schedule.sourceColorSpace &&
		loggedSMAAQualityName.Icmp( schedule.smaaQualityName ) == 0 ) {
		return;
	}

	common->Printf(
		"PostAA requested=%s cvar=%d effective=%s quality=%s source=%s(%dx%d) output=%s(%dx%d) sourceSpace=%s reason=%s smaaResources=%s\n",
		PostAAModeName( schedule.requestedMode ),
		schedule.requestedCvarValue,
		PostAAModeName( schedule.effectiveMode ),
		schedule.smaaQualityName,
		schedule.sourceImageName,
		schedule.sourceWidth,
		schedule.sourceHeight,
		schedule.outputImageName,
		schedule.outputWidth,
		schedule.outputHeight,
		schedule.sourceColorSpaceName,
		PostAADisableReasonName( schedule.disableReason ),
		smaaResourcesAvailable ? "ready" : "unavailable" );

	loggedRequestedValue = schedule.requestedCvarValue;
	loggedRequestedMode = schedule.requestedMode;
	loggedEffectiveMode = schedule.effectiveMode;
	loggedDisableReason = schedule.disableReason;
	loggedSMAAResourcesAvailable = smaaResourcesFlag;
	loggedSourceWidth = schedule.sourceWidth;
	loggedSourceHeight = schedule.sourceHeight;
	loggedOutputWidth = schedule.outputWidth;
	loggedOutputHeight = schedule.outputHeight;
	loggedSourceColorSpace = schedule.sourceColorSpace;
	loggedSMAAQualityName = schedule.smaaQualityName;
}

static bool openQ4_BuildPortalSkyCaptureView( const renderView_t *view, renderView_t *portalSkyView ) {
	return gameLocal.BuildPortalSkyRenderView( view, portalSkyView, gameRenderWorld );
}

static void UpdatePostAAWarningState( rvmGameRender_t& gameRender, int warningState ) {
	if ( gameRender.postAAWarningState == warningState ) {
		return;
	}

	gameRender.postAAWarningState = warningState;
	switch ( warningState ) {
	case OPENQ4_POST_AA_WARNING_UNSUPPORTED_MODE:
		common->Warning( "Unsupported r_postAA value; falling back to no post AA." );
		break;
	case OPENQ4_POST_AA_WARNING_UNAVAILABLE:
		common->Warning( "SMAA is enabled, but the current SMAA resources are unavailable. Falling back to no post AA." );
		break;
	case OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK:
		common->Warning( "SMAA is enabled, but r_usePostLightingStack disables the current SMAA path. Falling back to no post AA." );
		break;
	case OPENQ4_POST_AA_WARNING_INVALID_TARGET_SIZE:
		common->Warning( "SMAA is enabled, but the post-process render target sizes are invalid or inconsistent. Falling back to no post AA." );
		break;
	default:
		break;
	}
}

static const idMaterial* FindPostProcessMaterial( const char* primaryName, const char* fallbackName ) {
	const idMaterial* material = declManager->FindMaterial( primaryName, false );
	if ( IsUsablePostProcessMaterial( material ) || fallbackName == NULL ) {
		return material;
	}

	if ( material != NULL ) {
		common->Warning( "Post-process material '%s' defaulted during parse; trying fallback '%s'.",
			primaryName,
			( fallbackName != NULL ) ? fallbackName : "<none>" );
	}

	material = declManager->FindMaterial( fallbackName, false );
	if ( IsUsablePostProcessMaterial( material ) ) {
		return material;
	}

	if ( material != NULL ) {
		common->Warning( "Fallback post-process material '%s' defaulted during parse.", fallbackName );
	}

	return NULL;
}

static void openQ4_RenderSceneDirect( const renderView_t *view, idRenderWorld *renderWorld, idCamera *portalSky, int renderFlags ) {
	renderSystem->BindRenderTexture( nullptr, nullptr );
	renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );

	if ( portalSky ) {
		renderView_t portalSkyView = *view;
		portalSky->GetViewParms( &portalSkyView );
		renderWorld->RenderScene( &portalSkyView, ( renderFlags & ~RF_PRIMARY_VIEW ) | RF_DEFER_COMMAND_SUBMIT | RF_PORTAL_SKY );
	}

	renderWorld->RenderScene( view, renderFlags | RF_PENUMBRA_MAP );
}

static void openQ4_BindSceneRenderTexture( idRenderTexture *renderTexture ) {
	// Mark game-level 3D scene targets as feedback-capable so stock materials
	// that sample _currentRender, including heat haze, get an on-demand copy of
	// the active scene FBO. Fullscreen post passes bind with a NULL feedback
	// target to keep their source textures stable.
	renderSystem->BindRenderTexture( renderTexture, renderTexture );
}

static void openQ4_RenderSceneWorld( const renderView_t *view, idRenderWorld *renderWorld, idCamera *portalSky, int renderFlags ) {
	if ( portalSky ) {
		renderView_t portalSkyView = *view;
		portalSky->GetViewParms( &portalSkyView );
		renderWorld->RenderScene( &portalSkyView, ( renderFlags & ~RF_PRIMARY_VIEW ) | RF_DEFER_COMMAND_SUBMIT | RF_PORTAL_SKY );
	}

	renderWorld->RenderScene( view, renderFlags | RF_PENUMBRA_MAP );
}

static void openQ4_DrawFullScreenMaterial( const idMaterial *material ) {
	renderSystem->DrawStretchPic(
		0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
		0.0f, 1.0f, 1.0f, 0.0f,
		material );
}

static void openQ4_LabelGameRenderTargets( rvmGameRender_t& gameRender ) {
	renderSystem->SetRenderTextureDebugName( gameRender.forwardRenderPassRT, "openQ4 forward scene MSAA" );
	renderSystem->SetRenderTextureDebugName( gameRender.forwardRenderPassResolvedRT, "openQ4 forward scene resolved" );
	renderSystem->SetRenderTextureDebugName( gameRender.postProcessRT[2], "openQ4 PostAA SMAA scene source" );
	renderSystem->SetRenderTextureDebugName( gameRender.postProcessRT[1], "openQ4 PostAA SMAA edge/blend" );
	renderSystem->SetRenderTextureDebugName( gameRender.postProcessRT[0], "openQ4 PostAA SMAA weights/final" );
}

static bool openQ4_ExecuteSMAAPass( const openq4SMAAPass_t& pass ) {
	if ( pass.material == NULL ) {
		common->Warning( "PostAA SMAA %s pass has no material; skipping SMAA.", pass.passName );
		return false;
	}

	if ( !ValidateSMAAPassAliasing( pass ) ) {
		return false;
	}

	if ( pass.bindPostProcessSourceColorSpace ) {
		renderSystem->SetPostProcessSourceColorSpace( PostAASourceColorSpaceVector( pass.sourceColorSpace ) );
	}

	if ( pass.bindPostProcessSourceSize ) {
		if ( !SetSMAAPassSourceSize( pass.passName, pass.primarySource, pass.output ) ) {
			return false;
		}
	} else if ( !ValidateSMAAPassSize( pass ) ) {
		return false;
	}

	renderSystem->BindRenderTexture( pass.output, NULL );
	renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
	openQ4_DrawFullScreenMaterial( pass.material );
	return true;
}

static bool openQ4_ApplySMAA( rvmGameRender_t& gameRender, const openq4PostAASourceColorSpace_t sourceColorSpace, const openq4PostAAMode_t mode ) {
	if ( !ValidateSMAARenderTargetSizes( gameRender ) ) {
		common->Warning( "PostAA SMAA render target sizes changed unexpectedly; falling back to no post AA." );
		return false;
	}
	LogSMAARenderTargetSizes( gameRender );
	const openq4SMAAResources_t resources = BuildSMAAResources( gameRender );
	LogSMAAResourceOwnership( resources, sourceColorSpace, mode );
	renderSystem->SetPostProcessSMAAQuality( PostAASMAAQualityVector( mode ) );

	openq4SMAAPass_t passes[4];
	memset( passes, 0, sizeof( passes ) );

	passes[0].passName = "edge";
	passes[0].material = gameRender.smaaEdgePostProcessMaterial;
	passes[0].primarySource = resources.sceneSource;
	passes[0].primarySourceName = resources.sceneSourceName;
	passes[0].output = resources.edgeBlend;
	passes[0].outputName = resources.edgeBlendName;
	passes[0].clearBeforeDraw = true;
	passes[0].bindPostProcessSourceSize = true;
	passes[0].bindPostProcessSourceColorSpace = true;
	passes[0].sourceColorSpace = sourceColorSpace;

	passes[1].passName = "weights";
	passes[1].material = gameRender.smaaWeightsPostProcessMaterial;
	passes[1].primarySource = resources.edgeBlend;
	passes[1].primarySourceName = resources.edgeBlendName;
	passes[1].output = resources.weightsFinal;
	passes[1].outputName = resources.weightsFinalName;
	passes[1].clearBeforeDraw = true;
	passes[1].bindPostProcessSourceSize = true;

	passes[2].passName = "blend";
	passes[2].material = gameRender.smaaBlendPostProcessMaterial;
	passes[2].primarySource = resources.sceneSource;
	passes[2].primarySourceName = resources.sceneSourceName;
	passes[2].secondarySource = resources.weightsFinal;
	passes[2].secondarySourceName = resources.weightsFinalName;
	passes[2].output = resources.edgeBlend;
	passes[2].outputName = resources.edgeBlendName;
	passes[2].clearBeforeDraw = true;
	passes[2].bindPostProcessSourceSize = true;

	passes[3].passName = "normalize";
	passes[3].material = gameRender.copyPostProcess1Material;
	passes[3].primarySource = resources.edgeBlend;
	passes[3].primarySourceName = resources.edgeBlendName;
	passes[3].output = resources.weightsFinal;
	passes[3].outputName = resources.weightsFinalName;
	passes[3].clearBeforeDraw = true;
	passes[3].bindPostProcessSourceSize = false;

	for ( int i = 0; i < 4; i++ ) {
		if ( !openQ4_ExecuteSMAAPass( passes[i] ) ) {
			renderSystem->BindRenderTexture( NULL, NULL );
			return false;
		}
	}

	renderSystem->BindRenderTexture( NULL, NULL );
	return true;
}

static const idMaterial* openQ4_SelectFinalPostProcessMaterial( rvmGameRender_t& gameRender, bool blurEnabled ) {
	if ( blurEnabled ) {
		return gameRender.blurPostProcessMaterial;
	}
	if ( g_renderCasUpscale.GetBool() && gameRender.casPostProcessMaterial != NULL ) {
		return gameRender.casPostProcessMaterial;
	}
	return gameRender.noPostProcessMaterial;
}

/*
========================
idGameLocal::ShutdownGameRenderSystem
========================
*/
void idGameLocal::ShutdownGameRenderSystem( void ) {
	if ( renderSystem != NULL ) {
		renderSystem->SetPortalSkyCaptureViewCallback( NULL );
	}

	for ( int i = 0; i < 3; i++ ) {
		if ( gameRender.postProcessRT[i] != NULL ) {
			renderSystem->DestroyRenderTexture( gameRender.postProcessRT[i] );
			gameRender.postProcessRT[i] = NULL;
		}
	}

	if ( gameRender.forwardRenderPassRT != NULL ) {
		renderSystem->DestroyRenderTexture( gameRender.forwardRenderPassRT );
		gameRender.forwardRenderPassRT = NULL;
	}
	if ( gameRender.forwardRenderPassResolvedRT != NULL ) {
		renderSystem->DestroyRenderTexture( gameRender.forwardRenderPassResolvedRT );
		gameRender.forwardRenderPassResolvedRT = NULL;
	}

	gameRender.noPostProcessMaterial = NULL;
	gameRender.casPostProcessMaterial = NULL;
	gameRender.blurPostProcessMaterial = NULL;
	gameRender.blackPostProcessMaterial = NULL;
	gameRender.resolvePostProcessMaterial = NULL;
	gameRender.copyPostProcess1Material = NULL;
	gameRender.smaaEdgePostProcessMaterial = NULL;
	gameRender.smaaWeightsPostProcessMaterial = NULL;
	gameRender.smaaBlendPostProcessMaterial = NULL;
	gameRender.postProcessAvailable = false;
	gameRender.smaaAvailable = false;
	gameRender.renderTargetWidth = 0;
	gameRender.renderTargetHeight = 0;
	gameRender.videoRestartCount = ( renderSystem != NULL ) ? renderSystem->GetVideoRestartCount() : 0;
	gameRender.postAAWarningState = OPENQ4_POST_AA_WARNING_NONE;
}

/*
=======================================

Game Render

The engine renderer is designed to do two things, generate the geometry pass, and the shadow passes. The pipeline,
including post process, is now handled in the game code. This allows more granular control over how the final pixels,
are presented on screen based on whatever is going on in game.

=======================================
*/

/*
========================
idGameLocal::InitGameRenderSystem
========================
*/
void idGameLocal::InitGameRenderSystem(void) {
	ShutdownGameRenderSystem();

	if ( !renderSystem->IsOpenGLRunning() ) {
		return;
	}

	renderSystem->SetPortalSkyCaptureViewCallback( openQ4_BuildPortalSkyCaptureView );

	const int requestedMsaaSamples = Max( 0, cvarSystem->GetCVarInteger( "r_multiSamples" ) );

	{
		idImageOpts opts;
		// The legacy Quake 4 light stack depends on LDR framebuffer clamping
		// between blend/light-scale passes. Keep the game-level post chain LDR
		// so post AA preserves stock scene color instead of creating an HDR path.
		opts.format = FMT_RGBA8;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		opts.isPersistant = true;
		opts.width = renderSystem->GetScreenWidth();
		opts.height = renderSystem->GetScreenHeight();
		opts.numMSAASamples = requestedMsaaSamples;

		idImage *albedoImage = renderSystem->CreateImage("_forwardRenderAlbedo", &opts, TF_LINEAR);

		opts.numMSAASamples = requestedMsaaSamples;
		opts.format = FMT_DEPTH_STENCIL;
		idImage *depthImage = renderSystem->CreateImage("_forwardRenderDepth", &opts, TF_LINEAR);

		gameRender.forwardRenderPassRT = renderSystem->CreateRenderTexture(albedoImage, depthImage);
	}

	for(int i = 0; i < 3; i++)
	{
		idImageOpts opts;
		opts.format = FMT_RGBA8;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		opts.isPersistant = true;
		opts.width = renderSystem->GetScreenWidth();
		opts.height = renderSystem->GetScreenHeight();
		opts.numMSAASamples = 0;

		idImage* albedoImage = renderSystem->CreateImage(va("_postProcessAlbedo%d", i), &opts, TF_LINEAR);
		opts.format = FMT_DEPTH_STENCIL;
		idImage* depthImage = renderSystem->CreateImage(va("_postProcessDepth%d", i), &opts, TF_LINEAR);

		gameRender.postProcessRT[i] = renderSystem->CreateRenderTexture(albedoImage, depthImage, NULL);
	}

	{
		idImageOpts opts;
		opts.format = FMT_RGBA8;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		opts.isPersistant = true;
		opts.width = renderSystem->GetScreenWidth();
		opts.height = renderSystem->GetScreenHeight();
		opts.numMSAASamples = 0;

		idImage *albedoImage = renderSystem->CreateImage("_forwardRenderResolvedAlbedo", &opts, TF_LINEAR);
		// Match the forward pass depth attachment format so depth resolves/blits are valid
		// on drivers that reject DEPTH_STENCIL -> DEPTH-only copies.
		opts.format = FMT_DEPTH_STENCIL;
		idImage *depthImage = renderSystem->CreateImage("_forwardRenderResolvedDepth", &opts, TF_LINEAR);

		gameRender.forwardRenderPassResolvedRT = renderSystem->CreateRenderTexture(albedoImage, depthImage);
	}

	openQ4_LabelGameRenderTargets( gameRender );

	gameRender.blackPostProcessMaterial = FindPostProcessMaterial( "postprocess/black", "postprocess/openq4_black" );
	gameRender.noPostProcessMaterial = FindPostProcessMaterial( "postprocess/nopostprocess", "postprocess/openq4_nopostprocess" );
	gameRender.casPostProcessMaterial = FindPostProcessMaterial( "postprocess/casupscale", "postprocess/openq4_casupscale" );
	gameRender.blurPostProcessMaterial = FindPostProcessMaterial( "postprocess/blur", "postprocess/openq4_blur" );
	gameRender.resolvePostProcessMaterial = FindPostProcessMaterial( "postprocess/resolvepostprocess", "postprocess/openq4_resolvepostprocess" );
	gameRender.copyPostProcess1Material = FindPostProcessMaterial( "postprocess/copy_postprocess1", "postprocess/openq4_copy_postprocess1" );
	gameRender.smaaEdgePostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_edge", "postprocess/openq4_smaa_edge" );
	gameRender.smaaWeightsPostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_weights", "postprocess/openq4_smaa_weights" );
	gameRender.smaaBlendPostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_blend", "postprocess/openq4_smaa_blend" );
	gameRender.postProcessAvailable = (gameRender.noPostProcessMaterial != NULL) &&
		(gameRender.resolvePostProcessMaterial != NULL);
	gameRender.smaaAvailable = EvaluateSMAAAvailability( gameRender );
	{
		const openq4PostAASchedule_t postAASchedule = BuildPostAAPasses( gameRender, cvarSystem->GetCVarBool( "r_usePostLightingStack" ) );
		UpdatePostAAWarningState( gameRender, PostAAWarningStateForSchedule( postAASchedule ) );
		LogPostAASchedule( postAASchedule, gameRender.smaaAvailable );
	}
	if (!gameRender.postProcessAvailable) {
		common->Warning("Postprocess materials missing or invalid; falling back to direct render.");
	}

	if ( requestedMsaaSamples > 0 ) {
		common->Printf( "MSAA requested %d samples\n", requestedMsaaSamples );
	}

	gameRender.renderTargetWidth = renderSystem->GetScreenWidth();
	gameRender.renderTargetHeight = renderSystem->GetScreenHeight();
}

/*
========================
idGameLocal::ResizeRenderTextures
========================
*/
void idGameLocal::ResizeRenderTextures(int width, int height) {
	// Resize all of the different render textures.
	if ( gameRender.forwardRenderPassRT != NULL ) {
		renderSystem->ResizeRenderTexture( gameRender.forwardRenderPassRT, width, height );
	}
	if ( gameRender.forwardRenderPassResolvedRT != NULL ) {
		renderSystem->ResizeRenderTexture( gameRender.forwardRenderPassResolvedRT, width, height );
	}
	for ( int i = 0; i < 3; i++ ) {
		if ( gameRender.postProcessRT[i] != NULL ) {
			renderSystem->ResizeRenderTexture( gameRender.postProcessRT[i], width, height );
		}
	}

	gameRender.renderTargetWidth = width;
	gameRender.renderTargetHeight = height;
}

/*
====================
idGameLocal::RenderScene
====================
*/
void idGameLocal::RenderScene(const renderView_t *view, idRenderWorld *renderWorld, idCamera* portalSky, int renderFlags) {
	if ( view == NULL || renderWorld == NULL ) {
		return;
	}

	const bool previousUIViewportMode = renderSystem->GetUseUIViewportFor2D();
	renderSystem->SetUseUIViewportFor2D( false );

	const int currentVideoRestartCount = renderSystem->GetVideoRestartCount();
	if ( gameRender.videoRestartCount != currentVideoRestartCount ) {
		common->Printf( "Reinitializing game render targets after vid_restart (%d -> %d)\n",
			gameRender.videoRestartCount, currentVideoRestartCount );
		InitGameRenderSystem();
	}

	const int screenWidth = renderSystem->GetScreenWidth();
	const int screenHeight = renderSystem->GetScreenHeight();
	const bool haveScreenDimensions = ( screenWidth > 0 ) && ( screenHeight > 0 );

	bool needsTargetReinit = false;
	if ( haveScreenDimensions &&
		gameRender.forwardRenderPassRT != NULL &&
		gameRender.forwardRenderPassResolvedRT != NULL &&
		gameRender.postProcessRT[0] != NULL ) {
		if ( gameRender.renderTargetWidth != screenWidth ||
			gameRender.renderTargetHeight != screenHeight ) {
			needsTargetReinit = true;
		}
	}

	if ( needsTargetReinit ) {
		common->Printf(
			"Reinitializing game render targets after dimension/state change (%d x %d)\n",
			screenWidth,
			screenHeight );
		InitGameRenderSystem();
	}

	const bool canUsePostProcess = haveScreenDimensions &&
		gameRender.postProcessAvailable &&
		gameRender.forwardRenderPassRT != NULL &&
		gameRender.forwardRenderPassResolvedRT != NULL &&
		gameRender.postProcessRT[0] != NULL &&
		gameRender.resolvePostProcessMaterial != NULL;

	if ( !canUsePostProcess ) {
		// Fallback for stock Quake 4 assets or transient render-target invalidation.
		openQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
		renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
		return;
	}
	// Minimum render is used for screen captures(such as envcapture) calls, caller is responsible for all rendertarget setup.
	//if (view->minimumRender)
	//{
	//	RenderSky(view);
	//	if (view->cubeMapTargetImage)
	//	{
	//		renderView_t worldRefDef = *view;
	//		worldRefDef.cubeMapClearBuffer = false;
	//		renderWorld->RenderScene(&worldRefDef);
	//	}
	//	else
	//	{
	//		renderWorld->RenderScene(view);
	//	}
	//
	//	return;
	//}

	const int requestedMsaaSamples = Max( 0, cvarSystem->GetCVarInteger( "r_multiSamples" ) );
	const bool blurEnabled = IsSpecialEffectEnabled( SPECIAL_EFFECT_BLUR ) &&
		( gameRender.blurPostProcessMaterial != NULL );
	const bool wantsCAS = g_renderCasUpscale.GetBool() && gameRender.casPostProcessMaterial != NULL;
	openq4PostAASchedule_t postAASchedule = BuildPostAAPasses( gameRender, cvarSystem->GetCVarBool( "r_usePostLightingStack" ) );
	const bool wantsSMAA = PostAAModeUsesSMAA( postAASchedule.requestedMode );
	if ( wantsSMAA && gameRender.smaaAvailable ) {
		const bool previousSMAAAvailable = gameRender.smaaAvailable;
		gameRender.smaaAvailable = EvaluateSMAAAvailability( gameRender );
		if ( gameRender.smaaAvailable != previousSMAAAvailable ) {
			postAASchedule = BuildPostAAPasses( gameRender, cvarSystem->GetCVarBool( "r_usePostLightingStack" ) );
		}
	}
	UpdatePostAAWarningState( gameRender, PostAAWarningStateForSchedule( postAASchedule ) );
	LogPostAASchedule( postAASchedule, gameRender.smaaAvailable );

	const bool useSMAA = PostAAModeUsesSMAA( postAASchedule.effectiveMode );

	const bool canUseFastNoPost =
		g_renderFastNoPost.GetBool() &&
		requestedMsaaSamples <= 0 &&
		!blurEnabled &&
		!wantsSMAA &&
		!wantsCAS;

	if ( canUseFastNoPost ) {
		if ( g_renderFastNoPostDirect.GetBool() ) {
			openQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
			if ( g_renderCaptureCurrentRender.GetBool() ) {
				renderSystem->CaptureRenderToImage( "_currentRender" );
			}
			renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
			return;
		}

		openQ4_BindSceneRenderTexture( gameRender.forwardRenderPassResolvedRT );
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		openQ4_RenderSceneWorld( view, renderWorld, portalSky, renderFlags );
		renderSystem->BindRenderTexture( nullptr, nullptr );

		// The no-post path previously copied _forwardRenderResolvedAlbedo through
		// _postProcessAlbedo0 before presenting it. Presenting the resolved scene
		// material directly removes two full-screen passes without changing pixels.
		renderSystem->ClearRenderTarget( false, true, 1.0f, 0.0f, 0.0f, 0.0f );
		openQ4_DrawFullScreenMaterial( gameRender.resolvePostProcessMaterial );

		if ( g_renderCaptureCurrentRender.GetBool() ) {
			renderSystem->CaptureRenderToImage( "_currentRender" );
		}
		renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
		return;
	}

	// Render the scene to the forward render pass rendertexture.
	openQ4_BindSceneRenderTexture( gameRender.forwardRenderPassRT );
	{
		// Clear the color/depth buffers
		renderSystem->ClearRenderTarget(true, true, 1.0f, 0.0f, 0.0f, 0.0f);
	
		openQ4_RenderSceneWorld( view, renderWorld, portalSky, renderFlags );
	}
	renderSystem->BindRenderTexture(nullptr, nullptr);

	// Resolve our MSAA buffer.
	renderSystem->ResolveMSAA(
		gameRender.forwardRenderPassRT,
		gameRender.forwardRenderPassResolvedRT,
		blurEnabled || cvarSystem->GetCVarBool( "r_msaaResolveDepth" ) );

	// Resolve pass writes scene color to the post-process source buffer.
	renderSystem->BindRenderTexture(postAASchedule.resolveTarget, nullptr);
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		openQ4_DrawFullScreenMaterial( gameRender.resolvePostProcessMaterial );
	renderSystem->BindRenderTexture( nullptr, nullptr );
	if ( useSMAA ) {
		if ( !openQ4_ApplySMAA( gameRender, postAASchedule.sourceColorSpace, postAASchedule.effectiveMode ) ) {
			renderSystem->BindRenderTexture( gameRender.postProcessRT[0], nullptr );
			renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
			openQ4_DrawFullScreenMaterial( gameRender.resolvePostProcessMaterial );
			renderSystem->BindRenderTexture( nullptr, nullptr );
		}
	}

	const idMaterial* finalMaterial = openQ4_SelectFinalPostProcessMaterial( gameRender, blurEnabled );
	if ( finalMaterial == NULL ) {
		openQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
		renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
		return;
	}

	// SS_POST_PROCESS stages use depth testing; reset backbuffer depth each frame
	// so final full-screen composition is deterministic across drivers/devices.
	renderSystem->ClearRenderTarget( false, true, 1.0f, 0.0f, 0.0f, 0.0f );
	openQ4_DrawFullScreenMaterial( finalMaterial );

	if ( g_renderCaptureCurrentRender.GetBool() ) {
		renderSystem->CaptureRenderToImage( "_currentRender" );
	}
	renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
}
