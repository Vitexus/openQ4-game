// Game_render.cpp
//


#include "Game_local.h"
#include "../renderer/ImageOpts.h"

idCVar g_renderCasUpscale("g_renderCasUpscale", "0", CVAR_BOOL, "toggles the optional CAS post-process pass when a CAS material is available");
idCVar g_renderFastNoPost("g_renderFastNoPost", "1", CVAR_BOOL, "render through the direct no-post path when AA, blur, and CAS are disabled");
idCVar g_renderFastNoPostDirect("g_renderFastNoPostDirect", "1", CVAR_BOOL, "render directly to the backbuffer when the no-post path has no active post-processing work");
idCVar g_renderCaptureCurrentRender("g_renderCaptureCurrentRender", "0", CVAR_BOOL, "force an end-of-view _currentRender copy instead of relying on on-demand post-process capture");

enum openq4PostAAWarningState_t {
	OPENQ4_POST_AA_WARNING_NONE = 0,
	OPENQ4_POST_AA_WARNING_UNAVAILABLE,
	OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK
};

static bool IsUsablePostProcessMaterial( const idMaterial* material ) {
	return material != NULL && !material->TestMaterialFlag( MF_DEFAULTED );
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
		( gameRender.postProcessRT[1] != NULL );
}

static void UpdatePostAAWarningState( rvmGameRender_t& gameRender, int warningState ) {
	if ( gameRender.postAAWarningState == warningState ) {
		return;
	}

	gameRender.postAAWarningState = warningState;
	switch ( warningState ) {
	case OPENQ4_POST_AA_WARNING_UNAVAILABLE:
		common->Warning( "SMAA is enabled (r_postAA = 1), but the current SMAA resources are unavailable. Falling back to no post AA." );
		break;
	case OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK:
		common->Warning( "SMAA is enabled (r_postAA = 1), but r_usePostLightingStack disables the current SMAA path. Falling back to no post AA." );
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

static void OpenQ4_RenderSceneDirect( const renderView_t *view, idRenderWorld *renderWorld, idCamera *portalSky, int renderFlags ) {
	renderSystem->BindRenderTexture( nullptr, nullptr );
	renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );

	if ( portalSky ) {
		renderView_t portalSkyView = *view;
		portalSky->GetViewParms( &portalSkyView );
		renderWorld->RenderScene( &portalSkyView, ( renderFlags & ~RF_PRIMARY_VIEW ) | RF_DEFER_COMMAND_SUBMIT | RF_PORTAL_SKY );
	}

	renderWorld->RenderScene( view, renderFlags | RF_PENUMBRA_MAP );
}

static void OpenQ4_RenderSceneWorld( const renderView_t *view, idRenderWorld *renderWorld, idCamera *portalSky, int renderFlags ) {
	if ( portalSky ) {
		renderView_t portalSkyView = *view;
		portalSky->GetViewParms( &portalSkyView );
		renderWorld->RenderScene( &portalSkyView, ( renderFlags & ~RF_PRIMARY_VIEW ) | RF_DEFER_COMMAND_SUBMIT | RF_PORTAL_SKY );
	}

	renderWorld->RenderScene( view, renderFlags | RF_PENUMBRA_MAP );
}

static void OpenQ4_DrawFullScreenMaterial( const idMaterial *material ) {
	renderSystem->DrawStretchPic(
		0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
		0.0f, 1.0f, 1.0f, 0.0f,
		material );
}

/*
========================
idGameLocal::ShutdownGameRenderSystem
========================
*/
void idGameLocal::ShutdownGameRenderSystem( void ) {
	for ( int i = 0; i < 2; i++ ) {
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

	const int requestedMsaaSamples = Max( 0, cvarSystem->GetCVarInteger( "r_multiSamples" ) );

	{
		idImageOpts opts;
		// Keep scene lighting in FP16 until the final fullscreen presentation pass.
		opts.format = FMT_RGBA16F;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		opts.isPersistant = true;
		opts.width = renderSystem->GetScreenWidth();
		opts.height = renderSystem->GetScreenHeight();
		opts.numMSAASamples = requestedMsaaSamples;

		idImage *albedoImage = renderSystem->CreateImage("_forwardRenderAlbedo", &opts, TF_LINEAR);
		idImage *emissiveImage = renderSystem->CreateImage("_forwardRenderEmissive", &opts, TF_LINEAR);

		opts.numMSAASamples = requestedMsaaSamples;
		opts.format = FMT_DEPTH_STENCIL;
		idImage *depthImage = renderSystem->CreateImage("_forwardRenderDepth", &opts, TF_LINEAR);

		gameRender.forwardRenderPassRT = renderSystem->CreateRenderTexture(albedoImage, depthImage, emissiveImage);
	}

	for(int i = 0; i < 2; i++)
	{
		idImageOpts opts;
		// Ping-pong post-process buffers stay FP16 so bloom and grading retain highlight headroom.
		opts.format = FMT_RGBA16F;
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
		opts.format = FMT_RGBA16F;
		opts.colorFormat = CFM_DEFAULT;
		opts.numLevels = 1;
		opts.textureType = TT_2D;
		opts.isPersistant = true;
		opts.width = renderSystem->GetScreenWidth();
		opts.height = renderSystem->GetScreenHeight();
		opts.numMSAASamples = 0;

		idImage *albedoImage = renderSystem->CreateImage("_forwardRenderResolvedAlbedo", &opts, TF_LINEAR);
		idImage *emissiveImage = renderSystem->CreateImage("_forwardRenderResolvedEmissive", &opts, TF_LINEAR);
		// Match the forward pass depth attachment format so depth resolves/blits are valid
		// on drivers that reject DEPTH_STENCIL -> DEPTH-only copies.
		opts.format = FMT_DEPTH_STENCIL;
		idImage *depthImage = renderSystem->CreateImage("_forwardRenderResolvedDepth", &opts, TF_LINEAR);

		gameRender.forwardRenderPassResolvedRT = renderSystem->CreateRenderTexture(albedoImage, depthImage, emissiveImage);
	}

	gameRender.blackPostProcessMaterial = FindPostProcessMaterial( "postprocess/black", "postprocess/openq4_black" );
	gameRender.noPostProcessMaterial = FindPostProcessMaterial( "postprocess/nopostprocess", "postprocess/openq4_nopostprocess" );
	gameRender.casPostProcessMaterial = FindPostProcessMaterial( "postprocess/casupscale", "postprocess/openq4_casupscale" );
	gameRender.blurPostProcessMaterial = FindPostProcessMaterial( "postprocess/blur", "postprocess/openq4_blur" );
	gameRender.resolvePostProcessMaterial = FindPostProcessMaterial( "postprocess/resolvepostprocess", "postprocess/openq4_resolvepostprocess" );
	gameRender.smaaEdgePostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_edge", "postprocess/openq4_smaa_edge" );
	gameRender.smaaWeightsPostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_weights", "postprocess/openq4_smaa_weights" );
	gameRender.smaaBlendPostProcessMaterial = FindPostProcessMaterial( "postprocess/smaa_blend", "postprocess/openq4_smaa_blend" );
	gameRender.postProcessAvailable = (gameRender.noPostProcessMaterial != NULL) &&
		(gameRender.resolvePostProcessMaterial != NULL);
	gameRender.smaaAvailable = EvaluateSMAAAvailability( gameRender );
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
	for ( int i = 0; i < 2; i++ ) {
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
		OpenQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
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
	const bool wantsSMAA = ( cvarSystem->GetCVarInteger( "r_postAA" ) == 1 );
	const bool wantsCAS = g_renderCasUpscale.GetBool() && gameRender.casPostProcessMaterial != NULL;

	const bool canUseFastNoPost =
		g_renderFastNoPost.GetBool() &&
		requestedMsaaSamples <= 0 &&
		!blurEnabled &&
		!wantsSMAA &&
		!wantsCAS;

	if ( canUseFastNoPost ) {
		if ( g_renderFastNoPostDirect.GetBool() ) {
			OpenQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
			if ( g_renderCaptureCurrentRender.GetBool() ) {
				renderSystem->CaptureRenderToImage( "_currentRender" );
			}
			renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
			return;
		}

		renderSystem->BindRenderTexture( gameRender.forwardRenderPassResolvedRT, nullptr );
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		OpenQ4_RenderSceneWorld( view, renderWorld, portalSky, renderFlags );
		renderSystem->BindRenderTexture( nullptr, nullptr );

		// The no-post path previously copied _forwardRenderResolvedAlbedo through
		// _postProcessAlbedo0 before presenting it. Presenting the resolved scene
		// material directly removes two full-screen passes without changing pixels.
		renderSystem->ClearRenderTarget( false, true, 1.0f, 0.0f, 0.0f, 0.0f );
		OpenQ4_DrawFullScreenMaterial( gameRender.resolvePostProcessMaterial );

		if ( g_renderCaptureCurrentRender.GetBool() ) {
			renderSystem->CaptureRenderToImage( "_currentRender" );
		}
		renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
		return;
	}

	// Render the scene to the forward render pass rendertexture.
	renderSystem->BindRenderTexture(gameRender.forwardRenderPassRT, nullptr);
	{
		// Clear the color/depth buffers
		renderSystem->ClearRenderTarget(true, true, 1.0f, 0.0f, 0.0f, 0.0f);
	
		OpenQ4_RenderSceneWorld( view, renderWorld, portalSky, renderFlags );
	}
	renderSystem->BindRenderTexture(nullptr, nullptr);

	// Resolve our MSAA buffer.
	renderSystem->ResolveMSAA(
		gameRender.forwardRenderPassRT,
		gameRender.forwardRenderPassResolvedRT,
		blurEnabled || cvarSystem->GetCVarBool( "r_msaaResolveDepth" ) );

	// Resolve pass writes scene color to the post-process source buffer.
	renderSystem->BindRenderTexture(gameRender.postProcessRT[0], nullptr);
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		OpenQ4_DrawFullScreenMaterial( gameRender.resolvePostProcessMaterial );
	renderSystem->BindRenderTexture( nullptr, nullptr );

	if ( wantsSMAA && gameRender.smaaAvailable ) {
		gameRender.smaaAvailable = EvaluateSMAAAvailability( gameRender );
	}
	int postAAWarningState = OPENQ4_POST_AA_WARNING_NONE;
	if ( wantsSMAA ) {
		if ( cvarSystem->GetCVarBool( "r_usePostLightingStack" ) ) {
			postAAWarningState = OPENQ4_POST_AA_WARNING_POST_LIGHTING_STACK;
		} else if ( !gameRender.smaaAvailable ) {
			postAAWarningState = OPENQ4_POST_AA_WARNING_UNAVAILABLE;
		}
	}
	UpdatePostAAWarningState( gameRender, postAAWarningState );

	const bool useSMAA = wantsSMAA && ( postAAWarningState == OPENQ4_POST_AA_WARNING_NONE );
	if ( useSMAA ) {
		// Pass 1: edge detection into _postProcessAlbedo1.
		renderSystem->BindRenderTexture( gameRender.postProcessRT[1], nullptr );
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		renderSystem->DrawStretchPic(
			0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
			0.0f, 1.0f, 1.0f, 0.0f,
			gameRender.smaaEdgePostProcessMaterial );

		// Pass 2: blending weight calculation into _postProcessAlbedo0.
		renderSystem->BindRenderTexture( gameRender.postProcessRT[0], nullptr );
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		renderSystem->DrawStretchPic(
			0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
			0.0f, 1.0f, 1.0f, 0.0f,
			gameRender.smaaWeightsPostProcessMaterial );

		// Pass 3: neighborhood blending into _postProcessAlbedo1, then copy the
		// final SMAA output back into _postProcessAlbedo0 for the rest of the post stack.
		renderSystem->BindRenderTexture( gameRender.postProcessRT[1], nullptr );
		renderSystem->ClearRenderTarget( true, true, 1.0f, 0.0f, 0.0f, 0.0f );
		renderSystem->DrawStretchPic(
			0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT,
			0.0f, 1.0f, 1.0f, 0.0f,
			gameRender.smaaBlendPostProcessMaterial );
		renderSystem->CaptureRenderToImage( "_postProcessAlbedo0" );
		renderSystem->BindRenderTexture( nullptr, nullptr );
	}

	const idMaterial* finalMaterial = gameRender.noPostProcessMaterial;
	if ( blurEnabled ) {
		finalMaterial = gameRender.blurPostProcessMaterial;
	} else if ( g_renderCasUpscale.GetBool() && gameRender.casPostProcessMaterial != NULL ) {
		finalMaterial = gameRender.casPostProcessMaterial;
	}
	if ( finalMaterial == NULL ) {
		OpenQ4_RenderSceneDirect( view, renderWorld, portalSky, renderFlags );
		renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
		return;
	}

	// SS_POST_PROCESS stages use depth testing; reset backbuffer depth each frame
	// so final full-screen composition is deterministic across drivers/devices.
	renderSystem->ClearRenderTarget( false, true, 1.0f, 0.0f, 0.0f, 0.0f );
	OpenQ4_DrawFullScreenMaterial( finalMaterial );

	if ( g_renderCaptureCurrentRender.GetBool() ) {
		renderSystem->CaptureRenderToImage( "_currentRender" );
	}
	renderSystem->SetUseUIViewportFor2D( previousUIViewportMode );
}
