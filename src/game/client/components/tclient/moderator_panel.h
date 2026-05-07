#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_MODERATOR_PANEL_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_MODERATOR_PANEL_H

#include <base/process.h>
#include <engine/console.h>
#include <engine/shared/http.h>
#include <game/client/component.h>
#include <game/client/ui_rect.h>

#include <memory>
#include <string>
#include <vector>

class CModeratorPanel : public CComponent
{
public:
	struct CPlayerEntry
	{
		int m_Id;
		char m_aName[64];
		char m_aFullLine[768]; // full raw rcon status line, including timestamp + "I server:" prefix
	};

	struct CAttachment
	{
		std::string m_Url;
		std::string m_Filename;
	};

	struct CTodoEntry
	{
		std::string m_Name;
		std::string m_Reason;
		std::string m_PostUrl;
		std::string m_PostId;
		int64_t m_FirstMessageAt = 0; // Unix timestamp of the starter (first) message
		std::vector<CAttachment> m_vAttachments;
	};

	struct CStoredEntry
	{
		std::string m_Name;
		std::string m_Reason;
		std::string m_FullStatusLine;
	};

	struct CDownload
	{
		std::shared_ptr<CHttpRequest> m_pRequest;
		std::string m_PlayerName;
		std::string m_Filename;
	};

	struct CClickable
	{
		float m_X, m_Y, m_W, m_H;
		// 0 = copy ban (m_Data = PlayerId)
		// 1 = download attachments (m_Data = index into m_vPlayers)
		// 2 = switch active tab (m_Data = tab index)
		// 3 = copy detection status line (m_Data = index into detection log)
		// 4 = clear detection log
		// 5 = remove single detection entry (m_Data = index into detection log)
		// 6 = (unused, was open forum post)
		// 7 = open evidence folder
		// 8 = mark todo done (m_Data = index into m_vPlayers)
		// 9 = copy stored status line (m_Data = index into m_vStoredEntries)
		int m_Action;
		int m_Data;
	};

private:
	bool m_WasActive = false;
	bool m_OpenedMenus = false;
	bool m_WasScoreboardActive = false;
	bool m_KogCheckDone = false;
	int m_ActiveTab = 0; // 0 = players, 1 = detection log, 2 = stored
	float m_LogScrollY = 0.0f;
	PROCESS m_BridgeProcess = INVALID_PROCESS;
	int64_t m_LastJsonLoadTick = 0;
	int64_t m_LastJsonMtime = 0;
	float m_ScrollY = 0.0f;
	float m_PanelX = 0.0f, m_PanelY = 0.0f, m_PanelW = 0.0f, m_PanelH = 0.0f;
	std::vector<CPlayerEntry> m_vPlayers;
	std::vector<std::string> m_vTodos;
	std::vector<CTodoEntry> m_vTodoEntries;
	std::vector<CStoredEntry> m_vStoredEntries;
	std::vector<CDownload> m_vDownloads;
	std::vector<CClickable> m_vClickables;
	bool m_StoredEntriesDirty = false;
	int64_t m_LastStoredSaveTick = 0;

	static bool ParseStatusLine(const char *pLine, CPlayerEntry *pOut);
	void TryLoadTodoJson();
	void PollDownloads();
	void RenderDetectionLog(CUIRect Body);
	void RenderStoredLog(CUIRect Body);
	const CTodoEntry *FindTodo(const char *pName) const;

	static void SanitizeFilename(const char *pIn, char *pOut, size_t OutSize);
	static void JsonEscapeStr(const char *pIn, char *pOut, size_t OutSize);
	void BuildAttachmentPath(const char *pPlayerName, const char *pFilename, char *pOut, size_t OutSize) const;
	void MarkDone(int PlayerIndex);
	void OpenEvidenceFolder();
	void UpdateStoredEntry(const CPlayerEntry &Entry);
	void SaveStoredEntries() const;
	void LoadStoredEntries();

	static void ConTogglePanel(IConsole::IResult *pResult, void *pUserData);
	static void ConAddTodo(IConsole::IResult *pResult, void *pUserData);
	static void ConClearTodos(IConsole::IResult *pResult, void *pUserData);

public:
	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnInit() override;
	void OnReset() override;
	void OnShutdown() override;
	void OnRender() override;
	bool OnInput(const IInput::CEvent &Event) override;

	void StartDiscordBridge();
	bool IsBridgeRunning() const;

	void OnRconLine(const char *pLine);
	void OnKogServerConnect();

	bool IsActive() const;
	void SetActive(bool Active);
	void Toggle();

	bool IsTodo(const char *pName) const;
	void AddTodo(const char *pName);
	void ClearTodos();

	void CopyBanLine(int PlayerId);
	void CopyDetectionStatusLine(int LogIndex);
	void DownloadPlayerAttachments(const char *pPlayerName);

	const std::vector<CPlayerEntry> &Players() const { return m_vPlayers; }
};

#endif
