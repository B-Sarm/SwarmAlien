#pragma once

#include "Platforms/AkPlatformInfo.h"
#include "AkIOSPlatformInfo.generated.h"

UCLASS()
class UAkIOSPlatformInfo : public UAkPlatformInfo
{
	GENERATED_BODY()

public:
	UAkIOSPlatformInfo()
	{
		WwisePlatform = "iOS";
		Architecture = "iOS";
		bUsesStaticLibraries = true;
	}
};
