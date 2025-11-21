#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Sockets.h"
#include "MCPDynamicBuffer.h"

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
	void ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message);
	bool SendAll(TSharedPtr<FSocket> Client, const FString& Message);
	void SendHeartbeatIfNeeded(TSharedPtr<FSocket> Client, double& LastHeartbeatTime);
	FString BuildStatusPayload() const;
	FString BuildToolsPayload() const;

private:
	UUnrealMCPBridge* Bridge;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ClientSocket;
	FThreadSafeBool bRunning;
	double StartTimeSeconds;
};
