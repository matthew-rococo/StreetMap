#include "StreetMapImporting.h"
#include "StreetMapComponent.h"
#include "Elevation.h"

#include "DesktopPlatformModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Interfaces/IImageWrapperModule.h"
#include "SNotificationList.h"
#include "NotificationManager.h"
#include "ScopedTransaction.h"
#include "SpatialReferenceSystem.h"
#include "TiledMap.h"

#define LOCTEXT_NAMESPACE "StreetMapImporting"

static void ShowErrorMessage(const FText& MessageText)
{
	FNotificationInfo Info(MessageText);
	Info.ExpireDuration = 8.0f;
	Info.bUseLargeFont = false;
	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(SNotificationItem::CS_Fail);
		Notification->ExpireAndFadeout();
	}
}

static const FString& GetElevationCacheDir()
{
	static FString ElevationCacheDir;

	if (!ElevationCacheDir.Len())
	{
		auto UserTempDir = FPaths::ConvertRelativePathToFull(FDesktopPlatformModule::Get()->GetUserTempPath());
		ElevationCacheDir = FString::Printf(TEXT("%s%s"), *UserTempDir, TEXT("ElevationCache/"));
	}
	return ElevationCacheDir;
}

static FString GetCachedFilePath(uint32 X, uint32 Y, uint32 Z)
{
	FString FilePath = GetElevationCacheDir();
	FilePath.Append("elevation_");
	FilePath.AppendInt(Z);
	FilePath.Append("_");
	FilePath.AppendInt(X);
	FilePath.Append("_");
	FilePath.AppendInt(Y);
	FilePath.Append(".png");
	return FilePath;
}

static const int32 ExpectedElevationTileSize = 256;

class FCachedElevationFile
{
private:
	const FString URLTemplate = TEXT("http://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png");

	bool WasInitialized;
	bool WasDownloadASuccess;
	bool Failed;

	FDateTime StartTime;
	FTimespan TimeSpan;

	TSharedPtr<IHttpRequest> HttpRequest;

	bool UnpackElevation(const TArray<uint8>& RawData)
	{
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

		IImageWrapperPtr PngImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (PngImageWrapper.IsValid() && PngImageWrapper->SetCompressed(RawData.GetData(), RawData.Num()))
		{
			int32 BitDepth = PngImageWrapper->GetBitDepth();
			const ERGBFormat::Type Format = PngImageWrapper->GetFormat();
			const int32 Width = PngImageWrapper->GetWidth();
			const int32 Height = PngImageWrapper->GetHeight();

			if (Width != ExpectedElevationTileSize || Height != ExpectedElevationTileSize)
			{
				GWarn->Logf(ELogVerbosity::Error, TEXT("PNG file has wrong dimensions. Expected %dx%d"), ExpectedElevationTileSize, ExpectedElevationTileSize);
				return false;
			}

			if ((Format != ERGBFormat::RGBA && Format != ERGBFormat::BGRA) || BitDepth > 8)
			{
				GWarn->Logf(ELogVerbosity::Error, TEXT("PNG file contains elevation data in an unsupported format."));
				return false;
			}

			if (BitDepth <= 8)
			{
				BitDepth = 8;
			}

			const TArray<uint8>* RawPNG = nullptr;
			if (PngImageWrapper->GetRaw(Format, BitDepth, RawPNG))
			{
				const uint8* Data = RawPNG->GetData();
				Elevation.SetNumUninitialized(Width * Height);
				float* ElevationData = Elevation.GetData();
				const float* ElevationDataEnd = ElevationData + (Width * Height);

				while (ElevationData < ElevationDataEnd)
				{
					float ElevationValue = (Data[0] * 256.0f + Data[1] + Data[2] / 256.0f) - 32768.0f;

					*ElevationData = ElevationValue;

					ElevationData++;
					Data += 4;
				}				
			}

			return true;
		}

		return false;
	}

	void OnDownloadSucceeded(FHttpResponsePtr Response)
	{
		// unpack data
		if (Response.IsValid())
		{
			auto Content = Response->GetContent();
			if (!UnpackElevation(Content))
			{
				Failed = true;
				return;
			}

			// write data to cache
			FFileHelper::SaveArrayToFile(Content, *GetCachedFilePath(X, Y, Z));
		}

		WasDownloadASuccess = true;
	}

	void DownloadFile()
	{
		FString URL = FString::Printf(*URLTemplate, Z, X, Y);

		HttpRequest = FHttpModule::Get().CreateRequest();
		HttpRequest->SetVerb(TEXT("GET"));

		HttpRequest->SetURL(URL);
		if (!HttpRequest->ProcessRequest())
		{
			Failed = true;
		}
	}

	void Initialize()
	{
		WasInitialized = true;

		// try to load data from cache first
		{
			TArray<uint8> RawData;
			if (FFileHelper::LoadFileToArray(RawData, *GetCachedFilePath(X, Y, Z)))
			{
				if (UnpackElevation(RawData))
				{
					WasDownloadASuccess = true;
					return;
				}
			}
		}
		DownloadFile();
	}

protected:
	TArray<float> Elevation;
	uint32 X, Y, Z;

	friend class ElevationModel;

public:

	bool HasFinished() const
	{
		return WasDownloadASuccess || Failed;
	}

	bool Succeeded() const
	{
		return WasDownloadASuccess;
	}

	void CancelRequest()
	{
		Failed = true;
		HttpRequest->CancelRequest();
	}

	void Tick()
	{
		if (!WasInitialized)
		{
			Initialize();
		}

		if (WasDownloadASuccess || Failed) return;

		if (TimeSpan.GetSeconds() > 10)
		{
			GWarn->Logf(ELogVerbosity::Error, TEXT("Download time-out. Check your internet connection!"));
			Failed = true;
			HttpRequest->CancelRequest();
			return;
		}
		else
		{
			TimeSpan = FDateTime::UtcNow() - StartTime;
		}

		if (HttpRequest->GetStatus() == EHttpRequestStatus::Failed ||
			HttpRequest->GetStatus() == EHttpRequestStatus::Failed_ConnectionError)
		{
			GWarn->Logf(ELogVerbosity::Error, TEXT("Download connection failure. Check your internet connection!"));
			Failed = true;
			HttpRequest->CancelRequest();
			return;
		}

		if (HttpRequest->GetStatus() == EHttpRequestStatus::Succeeded)
		{
			OnDownloadSucceeded(HttpRequest->GetResponse());
			return;
		}

		HttpRequest->Tick(0);
	}

	FCachedElevationFile(uint32 X, uint32 Y, uint32 Z)
		: WasInitialized(false)
		, WasDownloadASuccess(false)
		, Failed(false)
		, StartTime(FDateTime::UtcNow())
		, X(X)
		, Y(Y)
		, Z(Z)
	{
	}
};

class FElevationModel
{
public:

	bool LoadElevationData(UStreetMapComponent* StreetMapComponent, const FStreetMapLandscapeBuildSettings& BuildSettings, FScopedSlowTask& SlowTask)
	{

		TArray<TSharedPtr<FCachedElevationFile>> FilesToDownload;

		// 1.) collect all elevation tiles needed based on StreetMap location and Landscape size
		{
			UStreetMap* StreetMap = StreetMapComponent->GetStreetMap();
			const FSpatialReferenceSystem SRS(StreetMap->GetOriginLongitude(), StreetMap->GetOriginLatitude());
			const FTiledMap TiledElevationMap = FTiledMap::MapzenElevation();

			const FVector2D SouthWest(-BuildSettings.RadiusInMeters, -BuildSettings.RadiusInMeters);
			const FVector2D NorthEast( BuildSettings.RadiusInMeters,  BuildSettings.RadiusInMeters);
			double South, West, North, East;
			if (!SRS.ToEPSG3857(SouthWest, West, South) || !SRS.ToEPSG3857(NorthEast, East, North))
			{
				ShowErrorMessage(LOCTEXT("ElevationBoundsInvalid", "Chosen elevation bounds are invalid. Stay within WebMercator bounds!"));
				return false;
			}

			// download highest resolution available
			const uint32 LevelIndex = TiledElevationMap.NumLevels - 1;
			FIntPoint SouthWestTileXY = TiledElevationMap.GetTileXY(West, South, LevelIndex);
			FIntPoint NorthEastTileXY = TiledElevationMap.GetTileXY(East, North, LevelIndex);

			for (int32 Y = SouthWestTileXY.Y; Y <= NorthEastTileXY.Y; Y++)
			{
				for (int32 X = SouthWestTileXY.X; X <= NorthEastTileXY.X; X++)
				{
					FilesToDownload.Add(MakeShared<FCachedElevationFile>(X, Y, LevelIndex));
				}
			}
		}

		// 2.) download the data from web service or disk if already cached
		const int32 NumFilesToDownload = FilesToDownload.Num();
		while (FilesToDownload.Num())
		{
			FHttpModule::Get().GetHttpManager().Tick(0);

			if (GWarn->ReceivedUserCancel())
			{
				break;
			}

			float Progress = 0.0f;
			for (auto FileToDownload : FilesToDownload)
			{
				FileToDownload->Tick();

				if (FileToDownload->HasFinished())
				{
					FilesToDownload.Remove(FileToDownload);
					Progress = 1.0f / (float)NumFilesToDownload;

					if (FileToDownload->Succeeded())
					{
						FilesDownloaded.Add(FileToDownload);
					}
					else
					{
						// We failed to download one file so cancel the rest because we cannot proceed without it.
						for (auto FileToCancel : FilesToDownload)
						{
							FileToCancel->CancelRequest();
						}
						FilesToDownload.Empty();
					}
					break;
				}
			}

			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("NumFilesDownloaded"), FText::AsNumber(FilesDownloaded.Num()));
			Arguments.Add(TEXT("NumFilesToDownload"), FText::AsNumber(NumFilesToDownload));
			SlowTask.EnterProgressFrame(Progress, FText::Format(LOCTEXT("DownloadingElevationModel", "Downloading Elevation Model ({NumFilesDownloaded} of {NumFilesToDownload})"), Arguments));

			if (Progress == 0.0f)
			{
				FPlatformProcess::Sleep(0.1f);
			}
		}

		if (FilesDownloaded.Num() < NumFilesToDownload)
		{
			ShowErrorMessage(LOCTEXT("DownloadElevationFailed", "Could not download all necessary elevation model files. See Log for details!"));
			return false;
		}

		return true;
	}

	void ReprojectData(UStreetMapComponent* StreetMapComponent, const FStreetMapLandscapeBuildSettings& BuildSettings, FScopedSlowTask& SlowTask, TArray<uint16>& OutElevationData)
	{
		SlowTask.EnterProgressFrame(0.0f, LOCTEXT("ReprojectingElevationModel", "Reprojecting Elevation Model"));

		const int32 SizeX = BuildSettings.RadiusInMeters * 2;
		const int32 SizeY = BuildSettings.RadiusInMeters * 2;
		const uint16 ZeroElevationOffset = 32768;

		UStreetMap* StreetMap = StreetMapComponent->GetStreetMap();
		const FSpatialReferenceSystem SRS(StreetMap->GetOriginLongitude(), StreetMap->GetOriginLatitude());
		const FTiledMap TiledElevationMap = FTiledMap::MapzenElevation();

		// sample elevation value for each height map vertex
		OutElevationData.SetNumUninitialized(SizeX * SizeY);
		uint16* Elevation = OutElevationData.GetData();
		for (int32 y = 0; y < SizeY; y++)
		{
			for (int32 x = 0; x < SizeX; x++)
			{
				int ElevationOffset = 0;

				double WebMercatorX, WebMercatorY;
				FVector2D VertexLocation(x, y);
				if (SRS.ToEPSG3857(VertexLocation, WebMercatorX, WebMercatorY))
				{
					// TODO: sample elevation at coordinates WebMercatorX/WebMercatorY
				}

				*Elevation = (uint16)(ZeroElevationOffset + ElevationOffset);
				Elevation++;
			}
		}
	}

private:
	TArray<TSharedPtr<FCachedElevationFile>> FilesDownloaded;
};


ALandscape* CreateLandscape(UWorld* World, const FStreetMapLandscapeBuildSettings& BuildSettings, const TArray<uint16>& ElevationData, FScopedSlowTask& SlowTask)
{
	SlowTask.EnterProgressFrame(0.0f, LOCTEXT("CreatingLandscape", "Filling Landscape with data"));

	FScopedTransaction Transaction(LOCTEXT("Undo", "Creating New Landscape"));
	ALandscape* Landscape = World->SpawnActor<ALandscape>();

	const int32 SizeX = BuildSettings.RadiusInMeters * 2;
	const int32 SizeY = BuildSettings.RadiusInMeters * 2;

	// create import layers
	TArray<FLandscapeImportLayerInfo> ImportLayers;
	{
		const auto& ImportLandscapeLayersList = BuildSettings.Layers;
		ImportLayers.Reserve(ImportLandscapeLayersList.Num());

		// Fill in LayerInfos array and allocate data
		for (const FLandscapeImportLayerInfo& UIImportLayer : ImportLandscapeLayersList)
		{
			FLandscapeImportLayerInfo ImportLayer = FLandscapeImportLayerInfo(UIImportLayer.LayerName);
			ImportLayer.LayerInfo = UIImportLayer.LayerInfo;
			ImportLayer.SourceFilePath = "";
			ImportLayer.LayerData = TArray<uint8>();
			ImportLayers.Add(MoveTemp(ImportLayer));
		}

		// @todo: fill the blend weights based on land use
		// for now Fill the first weight-blended layer to 100%
		{
			ImportLayers[0].LayerData.SetNumUninitialized(SizeX * SizeY);

			uint8* ByteData = ImportLayers[0].LayerData.GetData();
			for (int32 i = 0; i < SizeX * SizeY; i++)
			{
				ByteData[i] = 255;
			}
		}
	}

	Landscape->LandscapeMaterial = BuildSettings.Material;
	Landscape->Import(FGuid::NewGuid(), 
		-BuildSettings.RadiusInMeters, -BuildSettings.RadiusInMeters, 
		 BuildSettings.RadiusInMeters - 1, BuildSettings.RadiusInMeters - 1,
		1, 31, ElevationData.GetData(), nullptr,
		ImportLayers, ELandscapeImportAlphamapType::Additive);

	// automatically calculate a lighting LOD that won't crash lightmass (hopefully)
	// < 2048x2048 -> LOD0
	// >=2048x2048 -> LOD1
	// >= 4096x4096 -> LOD2
	// >= 8192x8192 -> LOD3
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);

	/*ULandscapeInfo* LandscapeInfo = Landscape->CreateLandscapeInfo();
	LandscapeInfo->UpdateLayerInfoMap(Landscape);

	// Import doesn't fill in the LayerInfo for layers with no data, do that now
	const TArray<FLandscapeImportLayerInfo>& ImportLandscapeLayersList = BuildSettings.Layers;
	for (int32 i = 0; i < ImportLandscapeLayersList.Num(); i++)
	{
		if (ImportLandscapeLayersList[i].LayerInfo != nullptr)
		{
			Landscape->EditorLayerSettings.Add(FLandscapeEditorLayerSettings(ImportLandscapeLayersList[i].LayerInfo));

			int32 LayerInfoIndex = LandscapeInfo->GetLayerInfoIndex(ImportLandscapeLayersList[i].LayerName);
			if (ensure(LayerInfoIndex != INDEX_NONE))
			{
				FLandscapeInfoLayerSettings& LayerSettings = LandscapeInfo->Layers[LayerInfoIndex];
				LayerSettings.LayerInfoObj = ImportLandscapeLayersList[i].LayerInfo;
			}
		}
	}*/

	return Landscape;
}


ALandscape* BuildLandscape(UStreetMapComponent* StreetMapComponent, UWorld* World, const FStreetMapLandscapeBuildSettings& BuildSettings)
{
	FScopedSlowTask SlowTask(2.0f, LOCTEXT("GeneratingLandscape", "Generating Landscape"));
	SlowTask.MakeDialog(true);

	FElevationModel ElevationModel;

	if (!ElevationModel.LoadElevationData(StreetMapComponent, BuildSettings, SlowTask))
	{
		return nullptr;
	}

	TArray<uint16> ElevationData;
	ElevationModel.ReprojectData(StreetMapComponent, BuildSettings, SlowTask, ElevationData);

	return CreateLandscape(World, BuildSettings, ElevationData, SlowTask);
}
