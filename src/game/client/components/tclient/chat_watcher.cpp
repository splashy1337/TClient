#include "chat_watcher.h"

#include <base/system.h>
#include <engine/demo.h>
#include <engine/external/json-parser/json.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <ctime>

#include <game/client/gameclient.h>

#include <cctype>
#include <cstdio>
#include <cstring>

static constexpr const char *WORDLIST_FILE = "chat_watcher_wordlist.txt";
static constexpr const char *LOG_FILE = "chat_watcher_log.json";

// Map a lowercase Cyrillic codepoint (U+0430..U+044F plus U+0451 ё) to a single Latin
// letter using phonetic equivalence. Returns 0 for codepoints we deliberately drop
// (ъ/ь — silent signs) or anything outside the Cyrillic block. Uppercase should be
// normalized to lowercase before calling.
static char CyrillicLowerToLatin(int Cp)
{
	switch(Cp)
	{
	case 0x0430: return 'a'; // а
	case 0x0431: return 'b'; // б
	case 0x0432: return 'v'; // в
	case 0x0433: return 'g'; // г
	case 0x0434: return 'd'; // д
	case 0x0435: return 'e'; // е
	case 0x0436: return 'j'; // ж (zh → j, keeps single-letter canonical)
	case 0x0437: return 'z'; // з
	case 0x0438: return 'i'; // и
	case 0x0439: return 'i'; // й
	case 0x043A: return 'k'; // к
	case 0x043B: return 'l'; // л
	case 0x043C: return 'm'; // м
	case 0x043D: return 'n'; // н
	case 0x043E: return 'o'; // о
	case 0x043F: return 'p'; // п
	case 0x0440: return 'r'; // р
	case 0x0441: return 's'; // с
	case 0x0442: return 't'; // т
	case 0x0443: return 'u'; // у
	case 0x0444: return 'f'; // ф
	case 0x0445: return 'h'; // х
	case 0x0446: return 'c'; // ц
	case 0x0447: return 'c'; // ч (ch → c)
	case 0x0448: return 's'; // ш (sh → s)
	case 0x0449: return 's'; // щ
	case 0x044A: return 0;   // ъ (hard sign, drop)
	case 0x044B: return 'i'; // ы
	case 0x044C: return 0;   // ь (soft sign, drop)
	case 0x044D: return 'e'; // э
	case 0x044E: return 'u'; // ю
	case 0x044F: return 'a'; // я
	case 0x0451: return 'e'; // ё
	}
	return 0;
}

// Canonicalize for slur/profanity matching. Designed to survive "a few misspelled
// letters or symbols": case, leet substitutions, inserted punctuation/spaces,
// repeated characters, and Cyrillic/Latin script mixing.
//
// Passes:
//   1. UTF-8 decode; Cyrillic → single-Latin phonetic (e.g. х→h, у→u, я→a)
//   2. ASCII upper → lower
//   3. Leet map: 0→o 1→i 3→e 4→a 5→s 7→t 8→b 9→g @→a $→s !→i +→t |→i (→c
//   4. Drop anything else (punctuation, spaces, symbols, non-mapped non-ASCII)
//   5. Collapse runs of the same character to one
//
// Examples:
//   "N!gg3r"         -> "niger"
//   "n i g g e r"    -> "niger"
//   "niiiigga"       -> "niga"
//   "хуй"            -> "hui"
//   "пizда"          -> "pizda"   (mixed Cyrillic+Latin)
//   "бlyаd"          -> "blyad"
// Wordlist entries are canonicalized the same way, so one stem covers all variants.
void CChatWatcher::Canonicalize(const char *pIn, std::string *pOut)
{
	pOut->clear();
	if(!pIn)
		return;
	pOut->reserve(strlen(pIn));
	char Prev = 0;
	const char *p = pIn;
	while(*p)
	{
		int Cp = str_utf8_decode(&p);
		if(Cp <= 0)
			break;
		char Mapped = 0;
		if(Cp < 128)
		{
			unsigned char c = (unsigned char)Cp;
			if(c >= 'A' && c <= 'Z')
				Mapped = (char)(c - 'A' + 'a');
			else if(c >= 'a' && c <= 'z')
				Mapped = (char)c;
			else
			{
				switch(c)
				{
				case '0': Mapped = 'o'; break;
				case '1': Mapped = 'i'; break;
				case '3': Mapped = 'e'; break;
				case '4': Mapped = 'a'; break;
				case '5': Mapped = 's'; break;
				case '7': Mapped = 't'; break;
				case '8': Mapped = 'b'; break;
				case '9': Mapped = 'g'; break;
				case '2': Mapped = '2'; break;
				case '6': Mapped = '6'; break;
				case '@': Mapped = 'a'; break;
				case '$': Mapped = 's'; break;
				case '!': Mapped = 'i'; break;
				case '+': Mapped = 't'; break;
				case '|': Mapped = 'i'; break;
				case '(': Mapped = 'c'; break;
				default: Mapped = 0; break;
				}
			}
		}
		else
		{
			int Lower = Cp;
			if(Cp >= 0x0410 && Cp <= 0x042F)
				Lower = Cp + 0x20; // uppercase Cyrillic → lowercase
			else if(Cp == 0x0401)
				Lower = 0x0451; // Ё → ё
			Mapped = CyrillicLowerToLatin(Lower);
		}
		if(Mapped == 0)
			continue;
		if(Mapped != Prev)
		{
			pOut->push_back(Mapped);
			Prev = Mapped;
		}
	}
}

bool CChatWatcher::IsKogServer() const
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;
	CServerInfo Info;
	Client()->GetServerInfo(&Info);
	if(Info.m_aName[0] == '\0')
		return false;
	return str_startswith(Info.m_aName, "|*KoG*|") != nullptr ||
	       str_startswith(Info.m_aName, "[A] |*KoG*|") != nullptr;
}

void CChatWatcher::ConWatcherAdd(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CChatWatcher *>(pUserData)->AddWord(pResult->GetString(0));
}

void CChatWatcher::ConWatcherRemove(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CChatWatcher *>(pUserData);
	const char *pWord = pResult->GetString(0);
	for(size_t i = 0; i < pSelf->m_vRawWords.size(); ++i)
	{
		if(str_comp_nocase(pSelf->m_vRawWords[i].c_str(), pWord) == 0)
		{
			pSelf->RemoveWordAt(i);
			return;
		}
	}
}

void CChatWatcher::ConWatcherClearLog(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CChatWatcher *>(pUserData)->ClearLog();
}

void CChatWatcher::ConWatcherReload(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CChatWatcher *>(pUserData);
	pSelf->LoadWordlist();
}

void CChatWatcher::OnConsoleInit()
{
	Console()->Register("mc_watcher_add", "s[word]", CFGFLAG_CLIENT, ConWatcherAdd, this,
		"Add a word to the chat watcher list");
	Console()->Register("mc_watcher_remove", "s[word]", CFGFLAG_CLIENT, ConWatcherRemove, this,
		"Remove a word from the chat watcher list");
	Console()->Register("mc_watcher_clear_log", "", CFGFLAG_CLIENT, ConWatcherClearLog, this,
		"Clear the chat watcher detection log");
	Console()->Register("mc_watcher_reload", "", CFGFLAG_CLIENT, ConWatcherReload, this,
		"Reload the chat watcher wordlist from disk");
}

void CChatWatcher::OnInit()
{
	// Seed wordlist — canonicalization (leet + Cyrillic→Latin + strip non-alnum +
	// collapse runs) means one stem covers many variants. Where the phonetic Cyrillic
	// mapping doesn't collide with a common Latin romanization (e.g. блядь→"blad"
	// vs "blyad"), both forms are seeded.
	static const char *const s_apSeed[] = {
		// English severe slurs
		"nigger", "nigga", "faggot", "retard", "cunt", "kike", "chink", "spic", "tranny",
		// Russian Cyrillic
		"\xd1\x85\xd1\x83\xd0\xb9",                                             // хуй
		"\xd0\xbf\xd0\xb8\xd0\xb7\xd0\xb4\xd0\xb0",                             // пизда
		"\xd0\xb1\xd0\xbb\xd1\x8f\xd0\xb4\xd1\x8c",                             // блядь
		"\xd0\xb5\xd0\xb1\xd0\xb0\xd1\x82\xd1\x8c",                             // ебать
		"\xd0\xbf\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80",                             // пидор
		"\xd0\xbf\xd0\xb8\xd0\xb4\xd0\xbe\xd1\x80\xd0\xb0\xd1\x81",             // пидорас
		"\xd1\x81\xd1\x83\xd0\xba\xd0\xb0",                                     // сука
		"\xd0\xbc\xd1\x83\xd0\xb4\xd0\xb0\xd0\xba",                             // мудак
		"\xd1\x85\xd0\xbe\xd1\x85\xd0\xbe\xd0\xbb",                             // хохол
		"\xd1\x87\xd1\x83\xd1\x80\xd0\xba\xd0\xb0",                             // чурка
		"\xd1\x85\xd1\x83\xd0\xb5\xd1\x81\xd0\xbe\xd1\x81",                     // хуесос
		"\xd0\xb4\xd0\xbe\xd0\xbb\xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb1",             // долбоеб
		"\xd1\x87\xd0\xbc\xd0\xbe",                                             // чмо
		"\xd0\xb6\xd0\xbe\xd0\xbf\xd0\xb0",                                     // жопа
		"\xd1\x83\xd0\xb5\xd0\xb1\xd0\xbe\xd0\xba",                             // уебок
		"\xd0\xb3\xd0\xb0\xd0\xbd\xd0\xb4\xd0\xbe\xd0\xbd",                     // гандон
		// Common Latin romanizations of Russian slurs that don't match Cyrillic canon
		"blyad", "blyat", "blya", "xui", "xyi", "cyka", "pidar", "pidaras",
		"xoxol", "khokhol", "churka", "chmo", "zhopa", "uebok"};

	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, WORDLIST_FILE, aPath, sizeof(aPath));
	time_t Created = 0, Modified = 0;
	const bool FileExists = fs_file_time(aPath, &Created, &Modified) == 0;

	bool ShouldSeed = !FileExists;
	if(FileExists)
	{
		LoadWordlist();
		// One-shot upgrade: if the file contains *only* the original 4-entry default
		// (from before Russian support landed), replace it with the expanded list.
		if(m_vRawWords.size() == 4 &&
			m_vRawWords[0] == "nigger" && m_vRawWords[1] == "nigga" &&
			m_vRawWords[2] == "faggot" && m_vRawWords[3] == "retard")
			ShouldSeed = true;
	}

	if(ShouldSeed)
	{
		m_vRawWords.clear();
		m_vCanonWords.clear();
		for(const char *pWord : s_apSeed)
		{
			std::string Canon;
			Canonicalize(pWord, &Canon);
			if(Canon.empty())
				continue;
			m_vRawWords.emplace_back(pWord);
			m_vCanonWords.push_back(std::move(Canon));
		}
		SaveWordlist();
	}

	LoadLog();
	m_Loaded = true;
}

void CChatWatcher::LoadWordlist()
{
	m_vRawWords.clear();
	m_vCanonWords.clear();

	void *pBuf = nullptr;
	unsigned Length = 0;
	if(!Storage()->ReadFile(WORDLIST_FILE, IStorage::TYPE_SAVE, &pBuf, &Length))
		return;

	const char *pText = static_cast<const char *>(pBuf);
	std::string Line;
	for(unsigned i = 0; i <= Length; ++i)
	{
		char c = (i < Length) ? pText[i] : '\n';
		if(c == '\r')
			continue;
		if(c == '\n')
		{
			// trim leading/trailing ASCII whitespace
			size_t Start = 0, End = Line.size();
			while(Start < End && (unsigned char)Line[Start] <= 0x20) ++Start;
			while(End > Start && (unsigned char)Line[End - 1] <= 0x20) --End;
			if(End > Start && Line[Start] != '#')
			{
				std::string Raw = Line.substr(Start, End - Start);
				std::string Canon;
				Canonicalize(Raw.c_str(), &Canon);
				if(!Canon.empty())
				{
					m_vRawWords.push_back(std::move(Raw));
					m_vCanonWords.push_back(std::move(Canon));
				}
			}
			Line.clear();
		}
		else
		{
			Line.push_back(c);
		}
	}
	free(pBuf);
}

void CChatWatcher::SaveWordlist()
{
	IOHANDLE File = Storage()->OpenFile(WORDLIST_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	const char *pHeader =
		"# Moderator Client chat watcher wordlist.\n"
		"# One entry per line. Lines starting with # are comments.\n"
		"# Matching is canonicalized (case-insensitive, leet-mapped, runs collapsed).\n";
	io_write(File, pHeader, (unsigned)strlen(pHeader));
	for(const auto &Word : m_vRawWords)
	{
		io_write(File, Word.c_str(), (unsigned)Word.size());
		io_write(File, "\n", 1);
	}
	io_close(File);
}

void CChatWatcher::AddWord(const char *pWord)
{
	if(!pWord || pWord[0] == '\0')
		return;
	for(const auto &W : m_vRawWords)
	{
		if(str_comp_nocase(W.c_str(), pWord) == 0)
			return;
	}
	std::string Canon;
	Canonicalize(pWord, &Canon);
	if(Canon.empty())
		return;
	m_vRawWords.emplace_back(pWord);
	m_vCanonWords.emplace_back(std::move(Canon));
	SaveWordlist();
}

void CChatWatcher::RemoveWordAt(size_t Index)
{
	if(Index >= m_vRawWords.size())
		return;
	m_vRawWords.erase(m_vRawWords.begin() + Index);
	m_vCanonWords.erase(m_vCanonWords.begin() + Index);
	SaveWordlist();
}

void CChatWatcher::ClearLog()
{
	m_vLog.clear();
	SaveLog();
}

void CChatWatcher::RemoveLogEntry(size_t Index)
{
	if(Index >= m_vLog.size())
		return;
	m_vLog.erase(m_vLog.begin() + Index);
	SaveLog();
}

const char *CChatWatcher::JsonEscape(const char *pIn, std::string *pScratch)
{
	pScratch->clear();
	if(!pIn)
		return "";
	for(const unsigned char *p = reinterpret_cast<const unsigned char *>(pIn); *p; ++p)
	{
		unsigned char c = *p;
		switch(c)
		{
		case '"': pScratch->append("\\\""); break;
		case '\\': pScratch->append("\\\\"); break;
		case '\n': pScratch->append("\\n"); break;
		case '\r': pScratch->append("\\r"); break;
		case '\t': pScratch->append("\\t"); break;
		case '\b': pScratch->append("\\b"); break;
		case '\f': pScratch->append("\\f"); break;
		default:
			if(c < 0x20)
			{
				char aBuf[8];
				str_format(aBuf, sizeof(aBuf), "\\u%04x", c);
				pScratch->append(aBuf);
			}
			else
			{
				pScratch->push_back((char)c);
			}
			break;
		}
	}
	return pScratch->c_str();
}

void CChatWatcher::SaveLog()
{
	IOHANDLE File = Storage()->OpenFile(LOG_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	io_write(File, "{\"entries\":[", 12);
	for(size_t i = 0; i < m_vLog.size(); ++i)
	{
		const CDetection &E = m_vLog[i];
		std::string Srv, Demo, Name, Clan, Msg, Matched;
		if(i > 0)
			io_write(File, ",", 1);
		io_write(File, "\n  {", 4);
		char aNum[128];

		str_format(aNum, sizeof(aNum), "\"ts\":%lld", (long long)E.m_Timestamp);
		io_write(File, aNum, (unsigned)strlen(aNum));

		const char *pSrv = JsonEscape(E.m_Server.c_str(), &Srv);
		io_write(File, ",\"server\":\"", 11);
		io_write(File, pSrv, (unsigned)strlen(pSrv));
		io_write(File, "\"", 1);

		std::string MapEsc;
		const char *pMap = JsonEscape(E.m_MapName.c_str(), &MapEsc);
		io_write(File, ",\"map\":\"", 8);
		io_write(File, pMap, (unsigned)strlen(pMap));
		io_write(File, "\"", 1);

		const char *pDemo = JsonEscape(E.m_DemoFile.c_str(), &Demo);
		io_write(File, ",\"demo\":\"", 9);
		io_write(File, pDemo, (unsigned)strlen(pDemo));
		io_write(File, "\"", 1);

		str_format(aNum, sizeof(aNum), ",\"tick\":%d,\"id\":%d", E.m_DemoTick, E.m_SenderId);
		io_write(File, aNum, (unsigned)strlen(aNum));

		const char *pName = JsonEscape(E.m_SenderName.c_str(), &Name);
		io_write(File, ",\"name\":\"", 9);
		io_write(File, pName, (unsigned)strlen(pName));
		io_write(File, "\"", 1);

		const char *pClan = JsonEscape(E.m_SenderClan.c_str(), &Clan);
		io_write(File, ",\"clan\":\"", 9);
		io_write(File, pClan, (unsigned)strlen(pClan));
		io_write(File, "\"", 1);

		str_format(aNum, sizeof(aNum), ",\"country\":%d,\"team\":%d", E.m_SenderCountry, E.m_SenderTeam);
		io_write(File, aNum, (unsigned)strlen(aNum));

		const char *pMsg = JsonEscape(E.m_Message.c_str(), &Msg);
		io_write(File, ",\"message\":\"", 12);
		io_write(File, pMsg, (unsigned)strlen(pMsg));
		io_write(File, "\"", 1);

		const char *pMatched = JsonEscape(E.m_MatchedWord.c_str(), &Matched);
		io_write(File, ",\"matched\":\"", 12);
		io_write(File, pMatched, (unsigned)strlen(pMatched));
		io_write(File, "\"", 1);

		if(!E.m_SenderStatusLine.empty())
		{
			std::string StatusEsc;
			const char *pStatus = JsonEscape(E.m_SenderStatusLine.c_str(), &StatusEsc);
			io_write(File, ",\"statusline\":\"", 15);
			io_write(File, pStatus, (unsigned)strlen(pStatus));
			io_write(File, "\"", 1);
		}

		io_write(File, "}", 1);
	}
	io_write(File, "\n]}\n", 4);
	io_close(File);
}

void CChatWatcher::LoadLog()
{
	m_vLog.clear();

	void *pBuf = nullptr;
	unsigned Length = 0;
	if(!Storage()->ReadFile(LOG_FILE, IStorage::TYPE_SAVE, &pBuf, &Length))
		return;

	json_settings Settings{};
	char aError[256];
	json_value *pRoot = json_parse_ex(&Settings, static_cast<json_char *>(pBuf), Length, aError);
	free(pBuf);
	if(!pRoot)
		return;
	if(pRoot->type != json_object)
	{
		json_value_free(pRoot);
		return;
	}
	const json_value &Entries = (*pRoot)["entries"];
	if(Entries.type != json_array)
	{
		json_value_free(pRoot);
		return;
	}
	for(unsigned i = 0; i < Entries.u.array.length; ++i)
	{
		const json_value &Item = *Entries.u.array.values[i];
		if(Item.type != json_object)
			continue;
		CDetection E;
		const json_value &Ts = Item["ts"];
		const json_value &Srv = Item["server"];
		const json_value &Map = Item["map"];
		const json_value &Demo = Item["demo"];
		const json_value &Tick = Item["tick"];
		const json_value &Id = Item["id"];
		const json_value &Name = Item["name"];
		const json_value &Clan = Item["clan"];
		const json_value &Country = Item["country"];
		const json_value &Team = Item["team"];
		const json_value &Msg = Item["message"];
		const json_value &Matched = Item["matched"];
		if(Ts.type == json_integer) E.m_Timestamp = Ts.u.integer;
		if(Srv.type == json_string) E.m_Server.assign(Srv.u.string.ptr, Srv.u.string.length);
		if(Map.type == json_string) E.m_MapName.assign(Map.u.string.ptr, Map.u.string.length);
		if(Demo.type == json_string) E.m_DemoFile.assign(Demo.u.string.ptr, Demo.u.string.length);
		if(Tick.type == json_integer) E.m_DemoTick = (int)Tick.u.integer;
		if(Id.type == json_integer) E.m_SenderId = (int)Id.u.integer;
		if(Name.type == json_string) E.m_SenderName.assign(Name.u.string.ptr, Name.u.string.length);
		if(Clan.type == json_string) E.m_SenderClan.assign(Clan.u.string.ptr, Clan.u.string.length);
		if(Country.type == json_integer) E.m_SenderCountry = (int)Country.u.integer;
		if(Team.type == json_integer) E.m_SenderTeam = (int)Team.u.integer;
		if(Msg.type == json_string) E.m_Message.assign(Msg.u.string.ptr, Msg.u.string.length);
		if(Matched.type == json_string) E.m_MatchedWord.assign(Matched.u.string.ptr, Matched.u.string.length);
		const json_value &StatusLine = Item["statusline"];
		if(StatusLine.type == json_string) E.m_SenderStatusLine.assign(StatusLine.u.string.ptr, StatusLine.u.string.length);
		m_vLog.push_back(std::move(E));
	}
	json_value_free(pRoot);
}

void CChatWatcher::AppendDetection(const CDetection &Det)
{
	m_vLog.push_back(Det);
	while(m_vLog.size() > MAX_LOG_ENTRIES)
		m_vLog.erase(m_vLog.begin());
	SaveLog();
}

void CChatWatcher::UpdateStatusLine(int SenderId, const char *pSenderName, const char *pStatusLine)
{
	bool Changed = false;
	for(auto &E : m_vLog)
	{
		if(E.m_SenderId == SenderId && E.m_SenderName == pSenderName && E.m_SenderStatusLine.empty())
		{
			E.m_SenderStatusLine = pStatusLine;
			Changed = true;
		}
	}
	if(Changed)
		SaveLog();
}

void CChatWatcher::OnChatMessage(int ClientId, const char *pMessage)
{
	if(!m_Loaded || !g_Config.m_McChatWatcher)
		return;
	if(!pMessage || pMessage[0] == '\0')
		return;
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	// Skip local player's own messages (per-user — a shared client install is safe).
	if(GameClient()->m_Snap.m_LocalClientId == ClientId)
		return;
	// KoG servers only.
	if(!IsKogServer())
		return;

	const CGameClient::CClientData &Sender = GameClient()->m_aClients[ClientId];
	if(Sender.m_aName[0] == '\0')
		return;

	std::string Canon;
	Canonicalize(pMessage, &Canon);
	if(Canon.empty() || m_vCanonWords.empty())
		return;

	const std::string *pHit = nullptr;
	for(const auto &W : m_vCanonWords)
	{
		if(!W.empty() && Canon.find(W) != std::string::npos)
		{
			pHit = &W;
			break;
		}
	}
	if(!pHit)
		return;

	CDetection Det;
	Det.m_Timestamp = (int64_t)::time(nullptr);
	CServerInfo Info;
	Client()->GetServerInfo(&Info);
	Det.m_Server = Info.m_aName;
	Det.m_MapName = Info.m_aMap;
	// Prefer MANUAL demo (explicit user recording), fall back to AUTO (auto-demos),
	// then RACE, then REPLAYS — whichever is actually writing right now.
	static const int s_aRecorderPriority[] = {RECORDER_MANUAL, RECORDER_AUTO, RECORDER_RACE, RECORDER_REPLAYS};
	for(int R : s_aRecorderPriority)
	{
		IDemoRecorder *pRec = Client()->DemoRecorder(R);
		if(pRec && pRec->IsRecording())
		{
			Det.m_DemoFile = pRec->CurrentFilename();
			Det.m_DemoTick = Client()->GameTick(0);
			break;
		}
	}
	Det.m_SenderId = ClientId;
	Det.m_SenderName = Sender.m_aName;
	Det.m_SenderClan = Sender.m_aClan;
	Det.m_SenderCountry = Sender.m_Country;
	Det.m_SenderTeam = Sender.m_Team;
	Det.m_Message = pMessage;
	Det.m_MatchedWord = *pHit;

	// Grab the full rcon status line immediately from the player list if it's already there.
	// The player is definitely still in the game at this point, so this is the most reliable path.
	for(const auto &P : GameClient()->m_ModeratorPanel.Players())
	{
		if(P.m_Id == ClientId && str_comp(P.m_aName, Sender.m_aName) == 0)
		{
			Det.m_SenderStatusLine = P.m_aFullLine;
			break;
		}
	}

	AppendDetection(Det);

	// Auto-pull rcon status so the full ban-quality line is captured at the moment
	// of offense, even if the offender disconnects seconds later. Throttled so spam
	// detections don't flood the server's rcon log.
	static constexpr int64_t AUTO_STATUS_THROTTLE_SECONDS = 10;
	if(Client()->RconAuthed())
	{
		const int64_t Now = Det.m_Timestamp;
		if(Now - m_LastAutoStatusTick >= AUTO_STATUS_THROTTLE_SECONDS)
		{
			Client()->Rcon("status");
			m_LastAutoStatusTick = Now;
		}
	}
}
