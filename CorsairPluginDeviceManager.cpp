#include "CorsairPluginDeviceManager.h"
#include "NetworkClient.h"

#include <future>
#include <fstream>
#include <unordered_set>

void ClientChanged(void* arg)
{
	CorsairPluginDeviceManager* pluginManager = static_cast<CorsairPluginDeviceManager*>(arg);
	pluginManager->DisconnectDevices();
	pluginManager->ConnectDevices();
}

CorsairPluginDeviceManager::CorsairPluginDeviceManager(void* pluginContext, _DeviceConnectionStatusChangeCallback callback,
	std::function<std::string(const std::string&)> imageHasher,
	std::function<std::string(const std::string&)> deviceHasher,
	std::function<std::wstring(const std::wstring&)> localPath)
	: mPluginContext(pluginContext)
	, mDeviceCallback(callback)
	, mNetworkClient(std::make_unique<NetworkClient>(mControllerList))
	, mImageHasher(imageHasher)
	, mDeviceHasher(deviceHasher)
	, mLocalFile(localPath)
	, mServicing(true)
{
	mNetworkClient->RegisterClientInfoChangeCallback(ClientChanged, this);
}

CorsairPluginDeviceManager::~CorsairPluginDeviceManager()
{
	Stop();

	if (mDeviceUpdateRequest.valid())
	{
		mDeviceUpdateRequest.wait();
	}
}

void CorsairPluginDeviceManager::Start()
{
	if (ReadJson())
	{
		if (mSettings.contains("OpenRGB"))
		{
			const auto& openRGB = mSettings["OpenRGB"];
			if (openRGB.contains("Host"))
			{
				mNetworkClient->SetIP(openRGB["Host"].get<std::string>().c_str());
			}
			if (openRGB.contains("Port"))
			{
				mNetworkClient->SetPort(openRGB["Port"]);
			}
			if (openRGB.contains("Client"))
			{
				mNetworkClient->SetName(openRGB["Client"].get<std::string>().c_str());
			}
		}

		mNetworkClient->StartClient();

		mServicing = true;
		mQueueServiceThread = std::make_unique<std::thread>(&CorsairPluginDeviceManager::ServiceThreadFunction, this);
	}
}

void CorsairPluginDeviceManager::Stop()
{
	mNetworkClient->StopClient();
	mServicing = false;
	if (mQueueServiceThread)
	{
		mQueueServiceThread->join();
	}
}

void CorsairPluginDeviceManager::ServiceThreadFunction()
{
	try
	{
		while (mServicing)
		{
			mQueueLock.lock();
			std::unique_ptr<SetColorData> colorQueue = nullptr;
			if (!mColorQueue.empty()) {
				colorQueue = std::move(mColorQueue.front());
				mColorQueue.pop();
			}
			mQueueLock.unlock();

			if (colorQueue) {
				_SetColor(colorQueue->mDeviceId.c_str(), colorQueue->mLEDs.size(), &colorQueue->mLEDs.at(0));
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	catch (...) {}
}

bool CorsairPluginDeviceManager::SetColor(const char* deviceId, std::int32_t size, cue::dev::plugin::LedColor* ledsColors)
{
	mQueueLock.lock();
	mColorQueue.push(std::make_unique<SetColorData>(deviceId, std::vector<cue::dev::plugin::LedColor>(ledsColors, ledsColors + size)));
	mQueueLock.unlock();
	return true;
}

bool CorsairPluginDeviceManager::_SetColor(const char* deviceId, std::int32_t size, cue::dev::plugin::LedColor* ledsColors)
{
	std::lock_guard<std::mutex> controllerLock(mNetworkClient->ControllerListMutex);
	std::lock_guard<std::mutex> deviceLock(mDeviceLock);

	auto it = mDeviceMap.find(deviceId);
	if (it != mDeviceMap.end())
	{
		auto controller = it->second->GetController();
		for (std::int32_t i = 0; i < size; ++i)
		{
			auto& ledColor = ledsColors[i];
			auto ledIt = it->second->GetInfo().ledMapping.find(ledColor.ledId);
			if (ledIt != it->second->GetInfo().ledMapping.end())
			{
				std::uint32_t zoneId = ledIt->second.first;
				std::uint32_t zoneIndex = ledIt->second.second;
				controller->SetLED(controller->zones.at(zoneId).start_idx + zoneIndex, ToRGBColor(ledColor.r, ledColor.g, ledColor.b));
			}
		}

		controller->UpdateLEDs();
		return true;
	}

	return false;
}

cue::dev::plugin::DeviceInfo* CorsairPluginDeviceManager::GetDeviceInfo(const char* deviceId)
{
	std::lock_guard<std::mutex> deviceLock(mDeviceLock);

	auto it = mDeviceMap.find(deviceId);
	if (it != mDeviceMap.end())
	{
		return it->second->CreateDeviceInfo();
	}

	return nullptr;
}

cue::dev::plugin::DeviceView* CorsairPluginDeviceManager::GetDeviceView(const char* deviceId, std::int32_t index)
{
	std::lock_guard<std::mutex> deviceLock(mDeviceLock);

	auto it = mDeviceMap.find(deviceId);
	if (it != mDeviceMap.end())
	{
		return it->second->CreateDeviceView(index);
	}

	return nullptr;
}

void CorsairPluginDeviceManager::UpdateDevices(std::unordered_set<std::string> deviceSet, bool notifyHost)
{
	// Phase 1: collect controller pointers under lock
	std::vector<std::pair<std::string, RGBController*>> toUpdate;
	{
		std::lock_guard<std::mutex> deviceLock(mDeviceLock);
		for (auto& deviceId : deviceSet)
		{
			auto it = mDeviceMap.find(deviceId);
			if (it != mDeviceMap.end())
			{
				toUpdate.emplace_back(deviceId, it->second->GetController());
			}
		}
	}

	// Phase 2: SetCustomMode WITHOUT holding mDeviceLock to avoid deadlock with _SetColor
	// (which takes ControllerListMutex then mDeviceLock)
	for (auto& entry : toUpdate)
	{
		entry.second->SetCustomMode();
	}

	// Phase 3: re-read zone data now that LED counts are refreshed, then notify
	std::vector<std::string> devicesToNotify;
	{
		std::lock_guard<std::mutex> deviceLock(mDeviceLock);
		for (auto& entry : toUpdate)
		{
			const std::string& deviceId = entry.first;
			RGBController* ctrl = entry.second;
			auto it = mDeviceMap.find(deviceId);
			if (it != mDeviceMap.end() && it->second->GetController() == ctrl)
			{
				if (it->second->ReadFromJson(mSettings, mDevices, true) && notifyHost)
				{
					devicesToNotify.push_back(deviceId);
				}
			}
		}
	}
	for (const auto& id : devicesToNotify)
	{
		mDeviceCallback(mPluginContext, id.c_str(), 1);
	}
}

bool CorsairPluginDeviceManager::ReadJson()
{
	std::ifstream devices(mLocalFile(L"devices.json").c_str());
	nlohmann::json jd;
	try
	{
		devices >> jd;
	}
	catch (...)
	{
		devices.close();
		return false;
	}

	devices.close();

	std::ifstream settings(mLocalFile(L"settings.json").c_str());
	nlohmann::json js;
	try
	{
		settings >> js;
	}
	catch (...)
	{
		settings.close();
		return false;
	}
	settings.close();

	mSettings = js;
	mDevices = jd;
	return true;
}

void CorsairPluginDeviceManager::ConnectDevices()
{
	std::unordered_set<std::string> deviceUpdate;

	{
		std::lock_guard<std::mutex> deviceLock(mDeviceLock);

		int deviceIndex = 0;
		for (auto controller : mControllerList)
		{
			std::unique_ptr<CorsairPluginDevice> device = std::make_unique<CorsairPluginDevice>(controller);
			device->SetImageHasher(mImageHasher);
			device->SetDeviceHasher(mDeviceHasher);
			device->SetDeviceIndex(deviceIndex++);

			// Initial read - zones may report 0 LEDs before SetCustomMode; UpdateDevices will refresh
			device->ReadFromJson(mSettings, mDevices);

			std::string deviceId = device->GetInfo().deviceId;
			if (!deviceId.empty())
			{
				for (auto& zone : device->GetResizeMap())
				{
					controller->ResizeZone(zone.first, zone.second);
				}

				mDeviceMap.emplace(deviceId, std::move(device));
				deviceUpdate.emplace(deviceId);
			}
		}
	}

	// Always route through UpdateDevices so SetCustomMode is called and
	// zone LED counts are refreshed before notifying iCUE
	if (!deviceUpdate.empty())
	{
		if (mDeviceUpdateRequest.valid())
		{
			mDeviceUpdateRequest.wait();
		}

		mDeviceUpdateRequest = std::async(std::launch::async, &CorsairPluginDeviceManager::UpdateDevices, this, deviceUpdate, true);
	}
}

void CorsairPluginDeviceManager::DisconnectDevices()
{
	std::vector<std::string> devicesToNotify;
	{
		std::lock_guard<std::mutex> deviceLock(mDeviceLock);
		for (auto& device : mDeviceMap)
		{
			devicesToNotify.push_back(device.first);
		}
		mDeviceMap.clear();
	}
	for (const auto& id : devicesToNotify)
	{
		mDeviceCallback(mPluginContext, id.c_str(), 0);
	}
}
