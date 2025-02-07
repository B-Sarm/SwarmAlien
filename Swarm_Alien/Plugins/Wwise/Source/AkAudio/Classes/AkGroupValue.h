#pragma once

#include "AkAudioType.h"
#include "AkGroupValue.generated.h"

UCLASS()
class AKAUDIO_API UAkGroupValue : public UAkAudioType
{
	GENERATED_BODY()

public:
#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "AkGroupValue")
	FGuid GroupID;
#endif

	UPROPERTY(VisibleAnywhere, Category = "AkGroupValue")
	uint32 GroupShortID;

public:
	void PostLoad() override;
	void BeginDestroy() override;

private:
	FString packagePath;
};
