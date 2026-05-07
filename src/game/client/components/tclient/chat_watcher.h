#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_CHAT_WATCHER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_CHAT_WATCHER_H

#include <engine/console.h>
#include <game/client/component.h>

#include <string>
#include <vector>

class CChatWatcher : public CComponent
{
public:
	struct CDetection
	{
		int64_t m_Timestamp = 0;
		std::string m_Server;
		std::string m_MapName;
		std::string m_DemoFile;
		int m_DemoTick = -1;
		int m_SenderId = -1;
		std::string m_SenderName;
		std::string m_SenderClan;
		int m_SenderCountry = -1;
		int m_SenderTeam = 0;
		std::string m_Message;
		std::string m_MatchedWord;
		std::string m_SenderStatusLine; // full rcon status line, captured from the status response
	};

private:
	bool m_Loaded = false;
	int64_t m_LastAutoStatusTick = 0;
	std::vector<std::string> m_vCanonWords;
	std::vector<std::string> m_vRawWords;
	std::vector<CDetection> m_vLog;

	static void Canonicalize(const char *pIn, std::string *pOut);
	bool IsKogServer() const;
	void LoadWordlist();
	void SaveWordlist();
	void LoadLog();
	void SaveLog();
	void AppendDetection(const CDetection &Det);
	static const char *JsonEscape(const char *pIn, std::string *pScratch);

	static void ConWatcherAdd(IConsole::IResult *pResult, void *pUserData);
	static void ConWatcherRemove(IConsole::IResult *pResult, void *pUserData);
	static void ConWatcherClearLog(IConsole::IResult *pResult, void *pUserData);
	static void ConWatcherReload(IConsole::IResult *pResult, void *pUserData);

public:
	static constexpr size_t MAX_LOG_ENTRIES = 500;

	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnInit() override;

	// Called from CChat::AddLine for every incoming chat line, including system/client msgs.
	// Safe with ClientId < 0 — those are ignored.
	void OnChatMessage(int ClientId, const char *pMessage);

	const std::vector<CDetection> &Log() const { return m_vLog; }
	const std::vector<std::string> &Words() const { return m_vRawWords; }

	void AddWord(const char *pWord);
	void RemoveWordAt(size_t Index);
	void ClearLog();
	void RemoveLogEntry(size_t Index);
	// Called by ModeratorPanel when a fresh rcon status line arrives for a player.
	// Stamps any detection entries that match id+name and have no status line yet.
	void UpdateStatusLine(int SenderId, const char *pSenderName, const char *pStatusLine);
};

#endif
