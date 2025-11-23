#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Sockets.h"
#include "MCPDynamicBuffer.h"
#include "Dom/JsonObject.h"

class UUnrealMCPBridge;

/**
 * Lightweight MCP-style TCP listener that exposes Unreal MCP commands directly from the plugin.
 * This is separate from the legacy JSON socket used by the Python bridge.
 */
class FMCPProtocolServerRunnable : public FRunnable
{
public:
	FMCPProtocolServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket);
	virtual ~FMCPProtocolServerRunnable();

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	void HandleClientConnection(TSharedPtr<FSocket> InClientSocket);
	void ProcessMessage(TSharedPtr<FSocket> Client, const TSharedPtr<FJsonObject>& Message);
	bool SendAll(TSharedPtr<FSocket> Client, const FString& Message);
	void SendHeartbeatIfNeeded(TSharedPtr<FSocket> Client, double& LastHeartbeatTime);
	FString BuildStatusPayload() const;
	TSharedPtr<FJsonObject> BuildToolsPayload() const;
	bool HandleHttpRequest(TSharedPtr<FSocket> Client, const FString& RawRequest);
	bool SendHttpResponse(TSharedPtr<FSocket> Client, const FString& Body, const FString& ContentType, int32 StatusCode = 200, const TMap<FString, FString>& ExtraHeaders = TMap<FString, FString>()) const;
	bool SendSseResponse(TSharedPtr<FSocket> Client, const FString& JsonPayload, int32 StatusCode = 200, const TMap<FString, FString>& ExtraHeaders = TMap<FString, FString>()) const;

private:
	UUnrealMCPBridge* Bridge;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ClientSocket;
	FThreadSafeBool bRunning;
	double StartTimeSeconds;
	FString SessionId;
};
