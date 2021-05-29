// Copyright 2021 Darby Costello. All Rights Reserved.
#include "VRTP.h"
#include "GameFramework/Pawn.h"
#include "PrimitiveSceneProxy.h"
#include "Misc/ScopeLock.h"
#include "EngineGlobals.h"
#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "IMotionController.h"
#include "PrimitiveSceneInfo.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "IXRSystemAssets.h"
#include "Components/StaticMeshComponent.h"
#include "MotionDelayBuffer.h"
#include "UObject/VRObjectVersion.h"
#include "UObject/UObjectGlobals.h" // for FindObject<>
#include "XRMotionControllerBase.h"
#include "IXRTrackingSystem.h"
#include "XRTrackingSystemBase.h"

//******************************************************
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Camera/CameraComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "IHeadMountedDisplay.h"
#include "Engine/GameEngine.h"
#include "Engine/Scene.h"
#include "Engine/LocalPlayer.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "VRTPMask.h"

DEFINE_LOG_CATEGORY_STATIC(LogMotionControllerComponent, Log, All);

namespace {
	/** This is to prevent destruction of motion controller components while they are
	in the middle of being accessed by the render thread */
	FCriticalSection CritSect;

	/** Console variable for specifying whether motion controller late update is used */
	TAutoConsoleVariable<int32> CVarEnableMotionControllerLateUpdate(
		TEXT("vr.EnableMotionControllerLateUpdate"),
		1,
		TEXT("This command allows you to specify whether the motion controller late update is applied.\n")
		TEXT(" 0: don't use late update\n")
		TEXT(" 1: use late update (default)"),
		ECVF_Cheat);
} // anonymous namespace


namespace LegacyMotionSources
{
	static bool GetSourceNameForHand(EControllerHand InHand, FName& OutSourceName)
	{
		UEnum* HandEnum = FindObject<UEnum>(ANY_PACKAGE, TEXT("EControllerHand"));
		if (HandEnum)
		{
			FString ValueName = HandEnum->GetNameStringByValue((int64)InHand);
			if (!ValueName.IsEmpty())
			{
				OutSourceName = *ValueName;
				return true;
			}
		}
		return false;
	}
}
//=============================================================================
UVRTunnellingPro::UVRTunnellingPro(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, RenderThreadComponentScale(1.0f, 1.0f, 1.0f)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bTickEvenWhenPaused = true;

	PlayerIndex = 0;
	MotionSource = FXRMotionControllerBase::HMDSourceId;
	bDisableLowLatencyUpdate = false;
	bHasAuthority = false;
	bAutoActivate = true;

	// ensure InitializeComponent() gets called
	bWantsInitializeComponent = true;
}

//=============================================================================
void UVRTunnellingPro::BeginDestroy()
{
	Super::BeginDestroy();
	if (ViewExtension.IsValid())
	{
		{
			FScopeLock ScopeLock(&CritSect);
			ViewExtension->MotionControllerComponent = NULL;
		}

		ViewExtension.Reset();
	}
}

void UVRTunnellingPro::CacheSettings()
{
	SkyboxBlueprintSwap = SkyboxBlueprint;
	CubeMapOverrideSwap = CubeMapOverride;
	PostProcessMaterialSwap = PostProcessMaterial;
	EffectColorSwap = EffectColor;
	EffectCoverageSwap = EffectCoverage;
	EffectFeatherSwap = EffectFeather;
	BackgroundModeSwap = BackgroundMode;
	ApplyEffectColorSwap = ApplyEffectColor;
	ForceEffectSwap = ForceEffect;
	MaskModeSwap = MaskMode;
	StencilIndexSwap = StencilIndex;
	bDirectionSpecificSwap = bDirectionSpecific;
	DirectionalVerticalStrengthSwap = DirectionalVerticalStrength;
	DirectionalHorizontalStrengthSwap = DirectionalHorizontalStrength;
	bUseAngularVelocitySwap = bUseAngularVelocity;
	AngularStrengthSwap = AngularStrength;
	AngularMinSwap = AngularMin;
	AngularMaxSwap = AngularMax;
	AngularSmoothingSwap = AngularSmoothing;
	bUseVelocitySwap = bUseVelocity;
	VelocityStrengthSwap = VelocityStrength;
	VelocityMinSwap = VelocityMin;
	VelocityMaxSwap = VelocityMax;
	VelocitySmoothingSwap = VelocitySmoothing;
	bUseAccelerationSwap = bUseAcceleration;
	AccelerationStrengthSwap = AccelerationStrength;
	AccelerationMinSwap = AccelerationMin;
	AccelerationMaxSwap = AccelerationMax;
	AccelerationSmoothingSwap = AccelerationSmoothing;
}

void UVRTunnellingPro::InitFromPreset()
{	
	if (Preset && bEnablePreset)
	{
		CacheSettings();
		SetPresetData(Preset);
	}

	if (!bEnablePreset) 
	{
		SkyboxBlueprint			= SkyboxBlueprintSwap;
		CubeMapOverride			= CubeMapOverrideSwap;
		PostProcessMaterial		= PostProcessMaterialSwap;
		EffectColor				= EffectColorSwap;
		EffectCoverage			= EffectCoverageSwap;
		EffectFeather			= EffectFeatherSwap;
		BackgroundMode			= BackgroundModeSwap;
		ApplyEffectColor		= ApplyEffectColorSwap;
		ForceEffect				= ForceEffectSwap;
		MaskMode				= MaskModeSwap;
		StencilIndex			= StencilIndexSwap;
		bUseAngularVelocity		= bUseAngularVelocitySwap;
		bDirectionSpecific	    = bDirectionSpecificSwap;
		DirectionalVerticalStrength   = DirectionalVerticalStrengthSwap;
		DirectionalHorizontalStrength = DirectionalHorizontalStrengthSwap;
		AngularStrength			= AngularStrengthSwap;
		AngularMin				= AngularMinSwap;
		AngularMax				= AngularMaxSwap;
		AngularSmoothing		= AngularSmoothingSwap;
		bUseVelocity			= bUseVelocitySwap;
		VelocityStrength		= VelocityStrengthSwap;
		VelocityMin				= VelocityMinSwap;
		VelocityMax				= VelocityMaxSwap;
		VelocitySmoothing		= VelocitySmoothingSwap;
		bUseAcceleration		= bUseAccelerationSwap;
		AccelerationStrength	= AccelerationStrengthSwap;
		AccelerationMin			= AccelerationMinSwap;
		AccelerationMax			= AccelerationMaxSwap;
		AccelerationSmoothing	= AccelerationSmoothingSwap;
	}
}

void UVRTunnellingPro::SetPresetData(UVRTPPresetData* NewPreset)
{
	if (NewPreset)
	{
		Preset = NewPreset;
		SkyboxBlueprint			= Preset->Data.SkyboxBlueprint;
		CubeMapOverride			= Preset->Data.CubeMapOverride;
		PostProcessMaterial		= Preset->Data.PostProcessMaterial;
		EffectColor				= Preset->Data.EffectColor;
		EffectCoverage			= Preset->Data.EffectCoverage;
		EffectFeather			= Preset->Data.EffectFeather;
		BackgroundMode			= Preset->Data.BackgroundMode;
		ApplyEffectColor		= Preset->Data.ApplyEffectColor;
		ForceEffect				= Preset->Data.ForceEffect;
		MaskMode				= Preset->Data.MaskMode;
		StencilIndex			= Preset->Data.StencilIndex;
		bDirectionSpecific      = Preset->Data.bDirectionSpecific;
		DirectionalVerticalStrength   = Preset->Data.DirectionalVerticalStrength;
		DirectionalHorizontalStrength = Preset->Data.DirectionalHorizontalStrength;
		bUseAngularVelocity		= Preset->Data.bUseAngularVelocity;
		AngularStrength			= Preset->Data.AngularStrength;
		AngularMin				= Preset->Data.AngularMin;
		AngularMax				= Preset->Data.AngularMax;
		AngularSmoothing		= Preset->Data.AngularSmoothing;
		bUseVelocity			= Preset->Data.bUseVelocity;
		VelocityStrength		= Preset->Data.VelocityStrength;
		VelocityMin				= Preset->Data.VelocityMin;
		VelocityMax				= Preset->Data.VelocityMax;
		VelocitySmoothing		= Preset->Data.VelocitySmoothing;
		bUseAcceleration		= Preset->Data.bUseAcceleration;
		AccelerationStrength	= Preset->Data.AccelerationStrength;
		AccelerationMin			= Preset->Data.AccelerationMin;
		AccelerationMax			= Preset->Data.AccelerationMax;
		AccelerationSmoothing	= Preset->Data.AccelerationSmoothing;
	}	
}


void UVRTunnellingPro::UpdatePostProcessSettings()
{
	if (PostProcessMID)
	{
		ApplyBackgroundMode();
		ApplyMaskMode();
		ApplyColor(ApplyEffectColor);
		SetFeather(EffectFeather);
		SetStencilMask(StencilIndex, true);
	}
}

void UVRTunnellingPro::ApplyPreset(UVRTPPresetData* NewPreset)
{
	if (NewPreset)
	{
		SetPresetData(NewPreset);
		if (CaptureInit) {
			UpdatePostProcessSettings();
		}
	}
}


void UVRTunnellingPro::SendRenderTransform_Concurrent()
{
	RenderThreadRelativeTransform = GetRelativeTransform();
	RenderThreadComponentScale = GetComponentScale();
	Super::SendRenderTransform_Concurrent();
}

//=============================================================================
void UVRTunnellingPro::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	if (!CaptureInit)
	{
		CaptureInit = true;
		InitCapture();
		InitSkybox();
	}

	if (IsActive())
	{
		FVector Position;
		FRotator Orientation;
		float WorldToMeters = GetWorld() ? GetWorld()->GetWorldSettings()->WorldToMeters : 100.0f;
		const bool bNewTrackedState = PollControllerState(Position, Orientation, WorldToMeters);
		if (bNewTrackedState)
		{
			SetRelativeLocationAndRotation(Position, Orientation);
		}

		// if controller tracking just kicked in 
		bTracked = bNewTrackedState;

		if (!ViewExtension.IsValid() && GEngine)
		{
			ViewExtension = FSceneViewExtensions::NewExtension<FViewExtension>(this);
		}

		// Send Actor directional vectors for skybox (cubemap) lookup
		if (PostProcessMID)
		{
			CalculateMotion(DeltaTime);
			PostProcessMID->SetVectorParameterValue(FName("Up"), GetOwner()->GetActorUpVector());
			PostProcessMID->SetVectorParameterValue(FName("Right"), GetOwner()->GetActorRightVector());
			PostProcessMID->SetVectorParameterValue(FName("Forward"), GetOwner()->GetActorForwardVector());
		}
	}
}

//=============================================================================
void UVRTunnellingPro::SetTrackingSource(const EControllerHand NewSource)
{
	if (LegacyMotionSources::GetSourceNameForHand(NewSource, MotionSource))
	{
		UWorld* MyWorld = GetWorld();
		if (MyWorld && MyWorld->IsGameWorld() && HasBeenInitialized())
		{
			FMotionDelayService::RegisterDelayTarget(this, PlayerIndex, MotionSource);
		}
	}
}

//=============================================================================
EControllerHand UVRTunnellingPro::GetTrackingSource() const
{
	EControllerHand Hand = EControllerHand::Left;
	FXRMotionControllerBase::GetHandEnumForSourceName(MotionSource, Hand);
	return Hand;
}

//=============================================================================
void UVRTunnellingPro::SetTrackingMotionSource(const FName NewSource)
{
	MotionSource = NewSource;

	UWorld* MyWorld = GetWorld();
	if (MyWorld && MyWorld->IsGameWorld() && HasBeenInitialized())
	{
		FMotionDelayService::RegisterDelayTarget(this, PlayerIndex, NewSource);
	}
}

//=============================================================================
void UVRTunnellingPro::SetAssociatedPlayerIndex(const int32 NewPlayer)
{
	PlayerIndex = NewPlayer;

	UWorld* MyWorld = GetWorld();
	if (MyWorld && MyWorld->IsGameWorld() && HasBeenInitialized())
	{
		FMotionDelayService::RegisterDelayTarget(this, NewPlayer, MotionSource);
	}
}

void UVRTunnellingPro::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FVRObjectVersion::GUID);

	Super::Serialize(Ar);

	if (Ar.CustomVer(FVRObjectVersion::GUID) < FVRObjectVersion::UseFNameInsteadOfEControllerHandForMotionSource)
	{
		LegacyMotionSources::GetSourceNameForHand(Hand_DEPRECATED, MotionSource);
	}
}

#if WITH_EDITOR
//=============================================================================
void UVRTunnellingPro::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
}

//=============================================================================
void UVRTunnellingPro::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	FProperty* PropertyThatChanged = PropertyChangedEvent.Property;
	const FName PropertyName = (PropertyThatChanged != nullptr) ? PropertyThatChanged->GetFName() : NAME_None;
	
	if (PropertyThatChanged->GetFName().ToString() == "Preset" || PropertyThatChanged->GetFName().ToString() == "bEnablePreset")
	{
		InitFromPreset();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

//=============================================================================
void UVRTunnellingPro::BeginPlay()
{
	Super::BeginPlay();
	CaptureInit = false;
}


//=============================================================================
void UVRTunnellingPro::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	Super::OnComponentDestroyed(bDestroyingHierarchy);

}

//=============================================================================
bool UVRTunnellingPro::PollControllerState(FVector& Position, FRotator& Orientation, float WorldToMetersScale)
{
	if (IsInGameThread())
	{
		// Cache state from the game thread for use on the render thread
		const AActor* MyOwner = GetOwner();
		const APawn* MyPawn = Cast<APawn>(MyOwner);
		bHasAuthority = MyPawn ? MyPawn->IsLocallyControlled() : (MyOwner->GetLocalRole() == ENetRole::ROLE_Authority);
	}

	if (bHasAuthority)
	{
		TArray<IMotionController*> MotionControllers = IModularFeatures::Get().GetModularFeatureImplementations<IMotionController>(IMotionController::GetModularFeatureName());
		for (auto MotionController : MotionControllers)
		{
			if (MotionController == nullptr)
			{
				continue;
			}

			CurrentTrackingStatus = MotionController->GetControllerTrackingStatus(PlayerIndex, MotionSource);
			if (MotionController->GetControllerOrientationAndPosition(PlayerIndex, MotionSource, Orientation, Position, WorldToMetersScale))
			{
				if (IsInGameThread())
				{
					InUseMotionController = MotionController;
					OnMotionControllerUpdated();
					InUseMotionController = nullptr;
				}
				return true;
			}
		}

		if (MotionSource == FXRMotionControllerBase::HMDSourceId)
		{
			IXRTrackingSystem* TrackingSys = GEngine->XRSystem.Get();
			if (TrackingSys)
			{
				FQuat OrientationQuat;
				if (TrackingSys->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, OrientationQuat, Position))
				{
					Orientation = OrientationQuat.Rotator();
					return true;
				}
			}
		}
	}
	return false;
}

//=============================================================================
UVRTunnellingPro::FViewExtension::FViewExtension(const FAutoRegister& AutoRegister, UVRTunnellingPro* InMotionControllerComponent)
	: FSceneViewExtensionBase(AutoRegister)
	, MotionControllerComponent(InMotionControllerComponent)
{}

//=============================================================================
void UVRTunnellingPro::FViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!MotionControllerComponent)
	{
		return;
	}

	// Set up the late update state for the controller component
	LateUpdate.Setup(MotionControllerComponent->CalcNewComponentToWorld(FTransform()), MotionControllerComponent, false);
}

//=============================================================================
void UVRTunnellingPro::FViewExtension::PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	if (!MotionControllerComponent)
	{
		return;
	}

	FTransform NewTransform;
	
	{
		FScopeLock ScopeLock(&CritSect);
		if (!MotionControllerComponent)
		{
			return;
		}

		// Find a view that is associated with this player.
		float WorldToMetersScale = -1.0f;
		for (const FSceneView* SceneView : InViewFamily.Views)
		{
			if (SceneView && SceneView->PlayerIndex == MotionControllerComponent->PlayerIndex)
			{
				WorldToMetersScale = SceneView->WorldToMetersScale;
				break;
			}
		}
		// If there are no views associated with this player use view 0.
		if (WorldToMetersScale < 0.0f)
		{
			check(InViewFamily.Views.Num() > 0);
			WorldToMetersScale = InViewFamily.Views[0]->WorldToMetersScale;
		}

		// Poll state for the most recent controller transform
		FVector Position;
		FRotator Orientation;
		if (!MotionControllerComponent->PollControllerState(Position, Orientation, WorldToMetersScale))
		{
			return;
		}

		NewTransform = FTransform(Orientation, Position, MotionControllerComponent->RenderThreadComponentScale);
		
	} // Release the lock on the MotionControllerComponent

}

void UVRTunnellingPro::FViewExtension::PostRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	check(IsInRenderingThread());

	if (!MotionControllerComponent)
	{
		return;
	}

	// LateUpdate.PostRender_RenderThread(); // not required in 4.24
}

bool UVRTunnellingPro::FViewExtension::IsActiveThisFrame(class FViewport* InViewport) const
{
	check(IsInGameThread());
	return MotionControllerComponent && !MotionControllerComponent->bDisableLowLatencyUpdate && CVarEnableMotionControllerLateUpdate.GetValueOnGameThread();
}

float UVRTunnellingPro::GetParameterValue(FName InName, bool& bValueFound)
{
	if (InUseMotionController)
	{
		return InUseMotionController->GetCustomParameterValue(MotionSource, InName, bValueFound);
	}
	bValueFound = false;
	return 0.f;
}


//*************************************************************

void UVRTunnellingPro::InitCapture()
{
	// Initialise Cube Capture
	SceneCaptureCube = NewObject<USceneCaptureComponentCube>(GetOwner());
	
	TC = NewObject<UTextureRenderTargetCube>();
	TC->ClearColor = FLinearColor::Black;
	TC->bHDR = false;
	TC->InitAutoFormat(1024);

	SceneCaptureCube->TextureTarget = TC;
	SceneCaptureCube->bCaptureOnMovement = false;
	SceneCaptureCube->bCaptureEveryFrame = false;
	SceneCaptureCube->bAutoActivate = true;
	SceneCaptureCube->CaptureStereoPass = EStereoscopicPass::eSSP_FULL;

	SceneCaptureCube->ShowFlags.SetAntiAliasing(false);
	SceneCaptureCube->ShowFlags.SetAtmosphere(false);
	SceneCaptureCube->ShowFlags.SetBloom(false);
	SceneCaptureCube->ShowFlags.SetBSP(false);
	SceneCaptureCube->ShowFlags.SetDeferredLighting(false);
	SceneCaptureCube->ShowFlags.SetEyeAdaptation(true);
	SceneCaptureCube->ShowFlags.SetFog(false);
	SceneCaptureCube->ShowFlags.SetVolumetricFog(false);

	UCameraComponent* PlayerCamera = GetOwner()->FindComponentByClass<UCameraComponent>();
	if (PlayerCamera != NULL)
	{
		IXRTrackingSystem* TrackingSys = GEngine->XRSystem.Get();
		if (TrackingSys)
		{
			PostProcessMID = UMaterialInstanceDynamic::Create(PostProcessMaterial, this);
			PlayerCamera->PostProcessSettings.AddBlendable(PostProcessMID, 1.0f);
			UpdatePostProcessSettings();
			IHeadMountedDisplay* HMD = GEngine->XRSystem->GetHMDDevice();
			HMD->GetFieldOfView(HFov, VFov);
			SceneCaptureCube->AttachToComponent(PlayerCamera, FAttachmentTransformRules::KeepRelativeTransform);
			PostProcessMID->SetTextureParameterValue(FName("TC"), TC);
		}
	}
}

void UVRTunnellingPro::InitSkybox()
{
	if (SkyboxBlueprint != NULL)
	{
		FVector Location = GetOwner()->GetActorLocation();
		FRotator Rotation = GetOwner()->GetActorRotation();
		FActorSpawnParameters SpawnInfo;
		SpawnInfo.Owner = GetOwner();
		Skybox = GetWorld()->SpawnActor(SkyboxBlueprint, &Location, &Rotation, SpawnInfo);

		if (Skybox != NULL)
		{
			Skybox->GetRootComponent()->AttachToComponent(GetOwner()->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
				TInlineComponentArray<UActorComponent*> MeshComponents(Skybox);
			for (int32 i = 0; i < MeshComponents.Num(); ++i)
			{
				UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(MeshComponents[i]);
				Mesh->SetOwnerNoSee(true);
			}
		}
		
		SceneCaptureCube->ShowOnlyActorComponents(Skybox);
		SceneCaptureCube->CaptureScene();
	}
}

void UVRTunnellingPro::SetBackgroundMode(EVRTPBackgroundMode NewBackgroundMode)
{
	BackgroundMode = NewBackgroundMode;
	ApplyBackgroundMode();
}

void UVRTunnellingPro::SetMaskMode(EVRTPMaskMode NewMaskMode)
{
	MaskMode = NewMaskMode;
	ApplyMaskMode();
}

void UVRTunnellingPro::ApplyBackgroundMode()
{
	switch (BackgroundMode)
	{
		case EVRTPBackgroundMode::MM_COLOR:
			PostProcessMID->SetScalarParameterValue(FName("BackgroundColor"), 1.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundSkybox"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundBlur"), 0.0f);

			if (Skybox != NULL) Skybox->SetActorHiddenInGame(true);
			break;

		case EVRTPBackgroundMode::MM_SKYBOX:
			PostProcessMID->SetScalarParameterValue(FName("BackgroundColor"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundSkybox"), 1.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundBlur"), 0.0f);

			if (CubeMapOverride != NULL)
			{
				PostProcessMID->SetScalarParameterValue(FName("CubeMapOverride"), 1.0f);
				PostProcessMID->SetTextureParameterValue(FName("CustomCubeMap"), CubeMapOverride);
			}
			else
			{
				PostProcessMID->SetScalarParameterValue(FName("CubeMapOverride"), 0.0f);
			}
			if (Skybox != NULL) Skybox->SetActorHiddenInGame(false);
			break;
	
		case EVRTPBackgroundMode::MM_BLUR:
			PostProcessMID->SetScalarParameterValue(FName("BackgroundColor"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundSkybox"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("BackgroundBlur"), 1.0f);
			if (Skybox != NULL) Skybox->SetActorHiddenInGame(true);
			break;
	}
}

void UVRTunnellingPro::ApplyMaskMode()
{
	switch (MaskMode)
	{
		case EVRTPMaskMode::MM_OFF:
			PostProcessMID->SetScalarParameterValue(FName("MaskOn"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskPortal"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskWindow"), 0.0f);
			break;

		case EVRTPMaskMode::MM_MASK:
			PostProcessMID->SetScalarParameterValue(FName("MaskOn"), 1.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskPortal"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskWindow"), 0.0f);
			break;

		case EVRTPMaskMode::MM_PORTAL:
			PostProcessMID->SetScalarParameterValue(FName("MaskOn"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskPortal"), 1.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskWindow"), 0.0f);
			break;

		case EVRTPMaskMode::MM_WINDOW:
			PostProcessMID->SetScalarParameterValue(FName("MaskOn"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskPortal"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("MaskWindow"), 1.0f);
			break;
	}
}

void UVRTunnellingPro::SetEffectColor(FLinearColor NewColor)
{
	EffectColor = NewColor;
	PostProcessMID->SetVectorParameterValue(FName("EffectColor"), FVector(EffectColor.R, EffectColor.G, EffectColor.B));
}

void UVRTunnellingPro::SetFeather(float NewFeather)
{
	EffectFeather = NewFeather;
	PostProcessMID->SetScalarParameterValue(FName("Feather"), EffectFeather);
}

void UVRTunnellingPro::SetStencilMask(int32 NewStencilIndex, bool UpdateMaskedObjects)
{
	StencilIndex = NewStencilIndex;
	PostProcessMID->SetScalarParameterValue(FName("MaskStencil"), (float)StencilIndex);
	if (UpdateMaskedObjects) ApplyStencilMasks();
}

void UVRTunnellingPro::UpdateMaskedObjects()
{
	ApplyStencilMasks();
}

void UVRTunnellingPro::ApplyStencilMasks()
{
	// Apply Custom Depth Stencil Index to all primitives within actors containing VRTPMask Component
	TArray<AActor*> AllActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), AllActors);
	for (AActor* Actor : AllActors)
	{
		if (Actor->FindComponentByClass(UVRTPMask::StaticClass()))
		{
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (UActorComponent* Component : Components)
			{
				UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
				if (Primitive)
				{
					if (Primitive->IsValidLowLevel()) {
						Primitive->SetCustomDepthStencilValue(StencilIndex);
						if (MaskMode != EVRTPMaskMode::MM_OFF) Primitive->SetRenderCustomDepth(true);
						else Primitive->SetRenderCustomDepth(false);
					}
				}
			}
		}
	}
}

void UVRTunnellingPro::ApplyColor(bool Enabled)
{
	ApplyEffectColor = Enabled;
	SetEffectColor(EffectColor);
	PostProcessMID->SetScalarParameterValue(FName("ApplyEffectColor"), (float)ApplyEffectColor);
}

void UVRTunnellingPro::CalculateMotion(float DeltaTime)
{
	float Radius = 0;
	float RadiusTarget = 0;
	float VelocityFinal = 0;
	FVector velocityVector = GetOwner()->GetActorLocation() - LastPosition;
	if (PostProcessMID != NULL)
	{
		if (!ForceEffect)
		{
			if (bUseAngularVelocity)
			{
				float AngleDelta = FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(GetOwner()->GetActorForwardVector(), LastForward))) / DeltaTime;
				// Check for divide by zero
				if (FMath::IsNearlyEqual(AngularMin, AngularMax, 0.001f)) AngleDelta = 0;
				else AngleDelta = (AngleDelta - AngularMin) / (AngularMax - AngularMin);
				float InterpSpeed = FMath::GetMappedRangeValueClamped(FVector2D(0, 1), FVector2D(1, 20), AngularSmoothing);
				AngleSmoothed = FMath::FInterpTo(AngleSmoothed, AngleDelta, DeltaTime, InterpSpeed);
				RadiusTarget += AngleSmoothed * (AngularStrength * 0.5);
				LastForward = GetOwner()->GetActorForwardVector();
			}

			if (bUseVelocity || bUseAcceleration)
			{
				float VelocityDelta = FVector::Distance(GetOwner()->GetActorLocation(), LastPosition) / DeltaTime;
				LastPosition = GetOwner()->GetActorLocation();

				if (bUseVelocity)
				{
					float InterpSpeed = FMath::GetMappedRangeValueClamped(FVector2D(0, 1), FVector2D(1, 20), VelocitySmoothing);
					VelocitySmoothed = FMath::FInterpTo(VelocitySmoothed, VelocityDelta, DeltaTime, InterpSpeed);

					// Check for divide by zero
					if (!FMath::IsNearlyEqual(VelocityMin, VelocityMax, 0.001f))
					{
						VelocityFinal = FMath::Clamp((VelocitySmoothed - VelocityMin) / (VelocityMax - VelocityMin), 0.0f, 1.0f);
					}
					RadiusTarget += VelocityFinal * VelocityStrength;
				}

				if (bUseAcceleration)
				{
					float AccelerationDelta = FMath::Abs(GetOwner()->GetVelocity().Size() - LastSpeed) / DeltaTime;
					LastSpeed = GetOwner()->GetVelocity().Size();

					// Check for divide by zero
					if (!FMath::IsNearlyEqual(AccelerationMin, AccelerationMax, 0.001f))
					{
						AccelerationDelta = FMath::Clamp((AccelerationDelta - AccelerationMin) / (AccelerationMax - AccelerationMin), 0.0f, 1.0f);
					}

					float InterpSpeed = FMath::GetMappedRangeValueClamped(FVector2D(0, 1), FVector2D(1, 20), AccelerationSmoothing);
					AccelerationSmoothed = FMath::FInterpTo(AccelerationSmoothed, AccelerationDelta, DeltaTime, InterpSpeed);
					RadiusTarget += AccelerationSmoothed * AccelerationStrength;
				}
			}

			if (bUseAngularVelocity || bUseAcceleration || bUseVelocity)
			{
				Radius = FMath::GetMappedRangeValueClamped(FVector2D(0, 1), FVector2D(1.5, 1 - EffectCoverage), RadiusTarget);
			}
			else
			{
				Radius = 1.5f;
			}
		}
		else
		{
			Radius = 0.3f;
		}

		PostProcessMID->SetScalarParameterValue(FName("Radius"), Radius);
		if (bDirectionSpecific) {
			PostProcessMID->SetScalarParameterValue(FName("XShift"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("YShift"), 0.0f);
			UCameraComponent* PlayerCamera = GetOwner()->FindComponentByClass<UCameraComponent>();
			if (PlayerCamera != NULL) {
				FVector cameraRight = PlayerCamera->GetRightVector();
				velocityVector.Normalize();
				FVector rightVelocity = velocityVector.ProjectOnTo(cameraRight);
				float strafeFactor = FVector::DotProduct(rightVelocity, cameraRight);
				FVector cameraForward = PlayerCamera->GetForwardVector();
				cameraForward.Normalize();
				PostProcessMID->SetScalarParameterValue(FName("YShift"), cameraForward.Z * ((1.5f - Radius) / 1.5f) * DirectionalVerticalStrength);
				PostProcessMID->SetScalarParameterValue(FName("XShift"), strafeFactor * DirectionalHorizontalStrength);
			}
		} else {
			PostProcessMID->SetScalarParameterValue(FName("XShift"), 0.0f);
			PostProcessMID->SetScalarParameterValue(FName("YShift"), 0.0f);
		}
	}
}