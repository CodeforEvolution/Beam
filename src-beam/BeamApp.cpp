/*
 * Copyright 2002-2006, project beam (http://sourceforge.net/projects/beam).
 * All rights reserved. Distributed under the terms of the GNU GPL v2.
 *
 * Authors:
 *		Oliver Tappe <beam@hirschkaefer.de>
 */


#include "BeamApp.h"

#include <algorithm>

#include <Alert.h>
#include <Beep.h>
#include <Deskbar.h>
#include <Roster.h>
#include <Screen.h>

#include "regexx.hh"

#include "BubbleHelper.h"
#include "ImageAboutWindow.h"

#include "BmBasics.h"
#include "BmBodyPartView.h"
#include "BmBusyView.h"
#include "BmDataModel.h"
#include "BmDeskbarView.h"
#include "BmEncoding.h"
#include "BmFilter.h"
#include "BmFilterChain.h"
#include "BmGuiRoster.h"
#include "BmIdentity.h"
#include "BmImapAccount.h"
#include "BmJobStatusWin.h"
#include "BmLogHandler.h"
#include "BmMailEditWin.h"
#include "BmMailFactory.h"
#include "BmMailFolderList.h"
#include "BmMailMonitor.h"
#include "BmMailMover.h"
#include "BmMailRef.h"
#include "BmMailView.h"
#include "BmMailViewWin.h"
#include "BmMainWindow.h"
#include "BmMsgTypes.h"
#include "BmNetUtil.h"
#include "BmPeople.h"
#include "BmRecvAccount.h"
#include "BmPopAccount.h"
#include "BmPrefs.h"
#include "BmPrefsWin.h"
#include "BmResources.h"
#include "BmSignature.h"
#include "BmSmtpAccount.h"
#include "BmStorageUtil.h"
#include "BmUtil.h"


static BmSlaveHandler sSlaveHandler;


class BmSlaveHandler {
public:
								BmSlaveHandler();
								~BmSlaveHandler();

			void				Run(const char* name, thread_func func,
									BMessage* message,
									uint16 minCountForThread = 10);
private:
			int32				fSlaveNum;
};


BmSlaveHandler::BmSlaveHandler()
	:
	fSlaveNum(1)
{
}


BmSlaveHandler::~BmSlaveHandler()
{
}


void
BmSlaveHandler::Run(const char* name, thread_func func, BMessage* message,
	uint16 minCountForThread)
{
	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT, reinterpret_cast<void**>(&refVect);
	if (refVect == NULL || refVect->size() <= minCountForThread) {
		// execute task in app-thread:
		func(message);
	} else {
		// start new thread for task:
		BmString tname(name);
		tname << "(" << fSlaveNum++ << ")";
		thread_id id = spawn_thread(func, tname.String(), B_NORMAL_PRIORITY,
			message);
		if (id < 0)
			throw BM_runtime_error(
				"BmSlaveHandler::Run(): Could not spawn thread");
		resume_thread(id);
	}
}


/*----------------------------------------------------------------------------*\
	MarkMailsAs( message)
		-	sets status for all mailref's contained in the given message.
		-	this is a thread-entry func.
\*----------------------------------------------------------------------------*/
static int32
MarkMailsAs(void* data)
{
	BMessage* message = static_cast<BMessage*>(data);
	if (message == NULL)
		return B_OK;

	int32 buttonPressed = 1;
		// mark all messages by default
	BmString newStatus = message->FindString(BeamApplication::MSG_STATUS);

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT, reinterpret_cast<void**>(&refVect);
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount > 1) {
		// First step, check if user has asked to mark as read and has selected
		// messages with advanced statii (like replied or forwarded). If so, we
		// ask the user about how to handle those:
		if (newStatus == BM_MAIL_STATUS_READ) {
			bool hasAdvanced = false;
			for (BmMailRefVect::iterator iter = refVect->begin();
				iter != refVect->end(); ++iter) {
				BmMailRef* mailRef = iter->Get();
				if (mailRef->Status() != BM_MAIL_STATUS_NEW
					&& mailRef->Status() != BM_MAIL_STATUS_READ) {
					hasAdvanced = true;
					break;
				}
			}

			if (hasAdvanced) {
				BmString alertMsg("Some of the selected messages are not new."
							  "\n\nShould Beam mark only the new messages "
							  "as being read?");
				BAlert* alert = new BAlert("Set mail status", alertMsg.String(),
					"Cancel", "Set status of all messages",
					"Set status of new messages only", B_WIDTH_AS_USUAL,
					B_OFFSET_SPACING, B_IDEA_ALERT);
				alert->SetShortcut(0, B_ESCAPE);
				buttonPressed = alert->Go();
			}
		}
	}

	if (buttonPressed != 0) {
		// tell sending ref-view that we are doing work:
		BMessenger sendingRefView;
		message->FindMessenger(BeamApplication::MSG_SENDING_REFVIEW,
			&sendingRefView);
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_SET_BUSY);

		uint32 i = 0;
		for (BmMailRefVect::iterator iter = refVect->begin();
			!gBeamApp->IsQuitting() && iter != refVect->end(); ++iter) {
			BmMailRef* mailRef = iter->Get();
			if (buttonPressed == 1 || mailRef->Status() == BM_MAIL_STATUS_NEW) {
				BM_LOG(BM_LogApp, BmString("marking mail <")
					<< mailRef->TrackerName() << "> as " << newStatus);
				mailRef->MarkAs( newStatus.String());
				if (++i % 100 == 0)
					snooze(500 * 1000);
						// give mail monitor a chance to catch up...
			}
		}

		// now tell sending ref-view that we are finished:
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_UNSET_BUSY);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	MoveMails(message)
		-	moves mails to a new folder.
		-	this is a thread-entry func.
\*----------------------------------------------------------------------------*/
static int32
MoveMails(void* data)
{
	BMessage* message = static_cast<BMessage*>(data);
	if (message == NULL)
		return B_OK;

	static int sJobNum = 1;

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t countFound = 0;
	if (refVect != NULL)
		countFound = refVect->size();

	if (countFound > 0) {
		// tell sending ref-view that we are doing work:
		BMessenger sendingRefView;
		message->FindMessenger(BeamApplication::MSG_SENDING_REFVIEW,
			&sendingRefView);
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_SET_BUSY);

		BMessage tempMessage(BM_JOBWIN_MOVEMAILS);
		BmString folderName = message->FindString(BmListModel::MSG_ITEMKEY);
		BmRef<BmListModelItem> itemRef = TheMailFolderList->FindItemByKey(
			folderName);
		BmMailFolder* folder = dynamic_cast<BmMailFolder*>(itemRef.Get());
		if (folder == NULL)
			return B_OK;

		BmString jobName = folder->DisplayKey();
		jobName << sJobNum++;
		tempMessage.AddString(BmJobModel::MSG_JOB_NAME, jobName.String());
		tempMessage.AddString(BmJobModel::MSG_MODEL, folder->Key().String());
		// now add a pointer to an array of entry_refs to message:
		struct entry_ref* refs = new(std::nothrow) entry_ref[countFound];
		if (refs == NULL)
			return B_NO_MEMORY;

		int32 index = 0;
		for (BmMailRefVect::iterator iter = refVect->begin();
			  !gBeamApp->IsQuitting() && iter != refVect->end(); ++iter) {
			BmMailRef* mailRef = iter->Get();
			BM_LOG(BM_LogApp, BmString("Asked to move mail <")
				<< mailRef->TrackerName() << "> to folder <"
				<< folder->DisplayKey() << ">");
			refs[index++] = mailRef->EntryRef();
		}

		tempMessage.AddPointer(BmMailMover::MSG_REFS, static_cast<void*>(refs));
		tempMessage.AddInt32(BmMailMover::MSG_REF_COUNT,
			static_cast<int32>(countFound));
			// message takes ownership of refs-array!
		TheJobStatusWin->PostMessage(&tempMessage);

		// now tell sending ref-view that we are finished:
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_UNSET_BUSY);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	TrashMails(message)
		-	moves mails to trash.
		-	this is a thread-entry func.
\*----------------------------------------------------------------------------*/
static int32
TrashMails(void* data)
{
	BMessage* message = static_cast<BMessage*>(data);
	if (message == NULL)
		return B_OK;

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t countFound = 0;
	if (refVect != NULL)
		countFound = refVect->size();

	if (countFound > 0) {
		BM_LOG(BM_LogApp, BmString("Asked to trash ") << countFound
			<< " mails");
		struct entry_ref* refs = new(std::nothrow) entry_ref[countFound];
		if (refs == NULL)
			return B_NO_MEMORY;

		// tell sending ref-view that we are doing work:
		BMessenger sendingRefView;
		message->FindMessenger(BeamApplication::MSG_SENDING_REFVIEW,
			&sendingRefView);
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_SET_BUSY);

		uint32 index = 0;
		for (BmMailRefVect::iterator iter = refVect->begin();
			!gBeamApp->IsQuitting() && iter != refVect->end(); ++iter) {
			BmMailRef* mailRef = iter->Get();
			refs[index++] = mailRef->EntryRef();
		}

		if (!gBeamApp->IsQuitting())
			MoveToTrash(refs, index);

		delete[] refs;

		// now tell sending ref-view that we are finished:
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage( BMM_UNSET_BUSY);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


struct OpenForEdit {
	void
	operator()(const BmRef<BmMail>& mail) {
		if (!gBeamApp->IsQuitting()) {
			BmMailEditWin* editWindow = BmMailEditWin::CreateInstance(mail.Get());
			if (editWindow != NULL)
				editWindow->Show();
		}
	}
};


/*----------------------------------------------------------------------------*\
	CreateMailsWithFactory( message, factory)
		-	handles a request to create new mails with given factory
\*----------------------------------------------------------------------------*/
static void
CreateMailsWithFactory(BMessage* message, BmMailRefVect* refVect,
	BmMailFactory* factory)
{
	if (message == NULL || refVect == NULL || factory == NULL)
		return;

	// tell sending ref-view that we are doing work:
	BMessenger sendingRefView;
	message->FindMessenger(BeamApplication::MSG_SENDING_REFVIEW,
		&sendingRefView);
	if (sendingRefView.IsValid())
		sendingRefView.SendMessage( BMM_SET_BUSY);

	for (uint32 index = 0; index < refVect->size(); ++index) {
		factory->AddBaseMailRef((*refVect)[index].Get());
	}
	factory->Produce();

	for_each(factory->TheMails.begin(), factory->TheMails.end(), OpenForEdit());
		// ...and open all created copies.

	// now tell sending ref-view that we are finished:
	if (sendingRefView.IsValid())
		sendingRefView.SendMessage( BMM_UNSET_BUSY);
}


/*----------------------------------------------------------------------------*\
	EditMailsAsNew( message)
		-	creates mails from given set (copies them and starts edit).
		-	this is a thread-entry func.
\*----------------------------------------------------------------------------*/
static int32
EditMailsAsNew(void* data)
{
	BMessage* message = static_cast<BMessage*>(data);
	if (message == NULL)
		return B_OK;

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount) {
		BM_LOG(BM_LogApp, BmString("Asked to edit ") << msgCount
			<< " mails as new.");
		BmCopyMailFactory factory;
		CreateMailsWithFactory(message, refVect, &factory);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	RedirectMails( message)
		-	redirects given mails.
		-	this is a thread-entry func.
\*----------------------------------------------------------------------------*/
static int32
RedirectMails(void* data)
{
	BMessage* message = static_cast<BMessage*>( data);
	if (message == NULL)
		return B_OK;

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount) {
		BM_LOG(BM_LogApp, BmString("Asked to redirect ") << msgCount
			<< " mails.");
		BmRedirectFactory factory;
		CreateMailsWithFactory( message, refVect, &factory);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	ForwardMails( message, join)
		-	forwards all mailref's contained in the given message.
		-	depending on the param join, multiple mails are joined into one
			single forward or one forward is generated for each mail.
\*----------------------------------------------------------------------------*/
static int32
ForwardMails( void* data)
{
	BMessage* message = static_cast<BMessage*>( data);
	if (message == NULL)
		return B_OK;

	int32 buttonPressed = 1;
	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount > 1) {
		// first step, ask user about how to forward multiple messages:
		BmString alertMsg("You have selected more than one message.\n\n"
					  "Should Beam join the message-bodies into one "
					  "single mail and forward that or would you prefer "
					  "to keep the messages separate?");
		BAlert* alert = new BAlert(
			"Forwarding multiple mails", alertMsg.String(),
		 	"Cancel", "Keep separate", "Join", B_WIDTH_AS_USUAL,
			B_OFFSET_SPACING, B_IDEA_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		buttonPressed = alert->Go();
	}

	if (buttonPressed > 0) {
		BM_LOG(BM_LogApp, BmString("Asked to forward ") << msgCount
			<< " mails.");

		bool join = (buttonPressed == 2);
		BmString selectedText = message->FindString(
			BeamApplication::MSG_SELECTED_TEXT);

		BmForwardMode forwardMode;
		switch (message->what) {
			case BMM_FORWARD_INLINE_ATTACH:
				forwardMode = BM_FORWARD_MODE_INLINE_ATTACH;
				break;
			case BMM_FORWARD_ATTACHED:
				forwardMode = BM_FORWARD_MODE_ATTACHED;
				break;
			default: // BMM_FORWARD_INLINE:
				forwardMode = BM_FORWARD_MODE_INLINE;
				break;
		};
		BmForwardFactory factory(forwardMode, join, selectedText);
		CreateMailsWithFactory(message, refVect, &factory);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	ReplyToMails( message, join)
		-	replies to all mailref's contained in the given message.
\*----------------------------------------------------------------------------*/
static int32
ReplyToMails(void* data)
{
	BMessage* message = static_cast<BMessage*>(data);
	if (message == NULL)
		return B_OK;

	int32 buttonPressed = 0;
	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount > 1) {
		// first step, ask user about how to forward multiple messages:
		BmString alertMsg("You have selected more than one message.\n\n"
						"Should Beam join the message-bodies into one "
						"single mail and reply to that or would you prefer "
						"to keep the messages separate?");
		BAlert* alert = new BAlert("Replying to multiple mails",
			alertMsg.String(), "Keep separate", "Join per recipient",
			"Join all", B_WIDTH_AS_USUAL, B_EVEN_SPACING, B_IDEA_ALERT);
		buttonPressed = alert->Go();
	}

	if (buttonPressed > -1) {
		BM_LOG(BM_LogApp, BmString("Asked to reply to ") << msgCount
			<< " mails.");

		bool join = (buttonPressed > 0);
		bool joinIntoOne = (buttonPressed == 2);
		BmString selectedText = message->FindString(
			BeamApplication::MSG_SELECTED_TEXT);

		BmReplyMode replyMode;
		switch (message->what) {
			case BMM_REPLY_LIST:
				replyMode = BM_REPLY_MODE_LIST;
				break;
			case BMM_REPLY_ORIGINATOR:
				replyMode = BM_REPLY_MODE_PERSON;
				break;
			case BMM_REPLY_ALL:
				replyMode = BM_REPLY_MODE_ALL;
				break;
			default: // BMM_REPLY:
				replyMode = BM_REPLY_MODE_SMART;
				break;
		};
		BmReplyFactory factory(replyMode, join, joinIntoOne, selectedText);
		CreateMailsWithFactory(message, refVect, &factory);
	}

	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


/*----------------------------------------------------------------------------*\
	PrintMails(message)
		-	prints all mailref's contained in the given message.
\*----------------------------------------------------------------------------*/
int32
PrintMails(void* data)
{
	BMessage* message = static_cast<BMessage*>( data);
	if (message == NULL)
		return B_OK;

	if (gBeamApp->fPrintSetup == NULL)
		gBeamApp->PageSetup();
	if (gBeamApp->fPrintSetup == NULL)
		return B_OK;

	BmMailRefVect* refVect = NULL;
	message->FindPointer(BeamApplication::MSG_MAILREF_VECT,
		reinterpret_cast<void**>(&refVect));
	size_t msgCount = 0;
	if (refVect != NULL)
		msgCount = refVect->size();

	if (msgCount) {
		// tell sending ref-view that we are doing work:
		BMessenger sendingRefView;
		message->FindMessenger(BeamApplication::MSG_SENDING_REFVIEW,
			&sendingRefView);
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage(BMM_SET_BUSY);

		gBeamApp->fPrintJob.SetSettings(new BMessage(*gBeamApp->fPrintSetup));
		status_t result = gBeamApp->fPrintJob.ConfigJob();
		if (result == B_OK) {
			delete gBeamApp->fPrintSetup;
			gBeamApp->fPrintSetup = gBeamApp->fPrintJob.Settings();

			int32 firstPage = gBeamApp->fPrintJob.FirstPage();
			int32 lastPage = gBeamApp->fPrintJob.LastPage();
			if ((lastPage - firstPage + 1) <= 0)
				goto out;

			// we create a hidden mail-view-window which is being used for
			// printing:
			BmMailViewWin* mailWin = BmMailViewWin::CreateInstance();
			mailWin->Hide();
			mailWin->Show();
			BmMailView* mailView = mailWin->MailView();

			// now get printable rect...
			BRect printableRect = gBeamApp->fPrintJob.PrintableRect();

			// ...and adjust mailview accordingly (to use the available space
			// effectively):
			if (mailView->LockLooper()) {
				mailWin->ResizeTo(printableRect.Width() + 8, 600);
				mailView->UnlockLooper();
			}
			mailView->BodyPartView()->IsUsedForPrinting(true);

			// now we start printing...
			gBeamApp->fPrintJob.BeginJob();
			int32 page = 1;
			for (uint32 mailIdx = 0;
				!gBeamApp->IsQuitting() && mailIdx < msgCount
					&& page <= lastPage;
				++mailIdx) {
				BmMailRef* mailRef = (*refVect)[mailIdx].Get();

				if (mailView->LockLooper()) {
					mailView->BodyPartView()->SetViewUIColor(
						B_DOCUMENT_BACKGROUND_COLOR);
					mailView->ShowMail(mailRef, false);
					mailView->UnlockLooper();
				}

				while (!mailView->IsDisplayComplete())
					snooze(50 * 1000);

				BRect currFrame = printableRect.OffsetToCopy(0, 0);
				BRect textRect = mailView->TextRect();
				float totalHeight = textRect.top
					+ mailView->TextHeight(0, 100000);
				float height = currFrame.Height();
				BPoint topOfLine = mailView->PointAt(mailView->OffsetAt(
					BPoint(5, currFrame.bottom)));
				currFrame.bottom = topOfLine.y - 1;

				while (!gBeamApp->IsQuitting() && page <= lastPage) {
					if (page >= firstPage) {
						gBeamApp->fPrintJob.DrawView(mailView, currFrame,
							BPoint(0,0));
						gBeamApp->fPrintJob.SpoolPage();
					}

					currFrame.top = currFrame.bottom + 1;
					currFrame.bottom = currFrame.top + height - 1;
					if (mailView->LockLooper()) {
						topOfLine = mailView->PointAt(mailView->OffsetAt(
							BPoint(5, currFrame.bottom)));
						mailView->UnlockLooper();
					}
					currFrame.bottom = topOfLine.y - 1;
					page++;

					if (currFrame.top >= totalHeight
						|| currFrame.Height() <= 1) {
						// end of current mail reached
						break;
					}
				}
			}

			gBeamApp->fPrintJob.CommitJob();
			mailWin->PostMessage(B_QUIT_REQUESTED);
		}

		// now tell sending ref-view that we are finished:
		if (sendingRefView.IsValid())
			sendingRefView.SendMessage( BMM_UNSET_BUSY);
	}

out:
	delete refVect;
		// freeing all references to mailrefs contained in vector
	delete message;

	return B_OK;
}


BeamApplication* gBeamApp = NULL;

static const char* BM_BEEP_EVENT = "New E-mail";

const char* BM_APP_SIG = "application/x-vnd.zooey-beam";
const char* BM_TEST_APP_SIG = "application/x-vnd.zooey-testbeam";
const char* const BM_DeskbarItemName = "Beam_DeskbarItem";

const char* const BeamApplication::MSG_MAILREF_VECT =		"bm:mrefv";
const char* const BeamApplication::MSG_STATUS = 			"bm:status";
const char* const BeamApplication::MSG_WHO_TO = 			"bm:to";
const char* const BeamApplication::MSG_OPT_FIELD =			"bm:optf";
const char* const BeamApplication::MSG_OPT_VALUE =			"bm:optv";
const char* const BeamApplication::MSG_SUBJECT = 			"bm:subj";
const char* const BeamApplication::MSG_SELECTED_TEXT = 		"bm:seltext";
const char* const BeamApplication::MSG_SENDING_REFVIEW =	"bm:srefv";
const char* const BeamApplication::MSG_ENCLOSE =			"bm:encl";


/*----------------------------------------------------------------------------*\
	BeamApplication()
		-	constructor
\*----------------------------------------------------------------------------*/
BeamApplication::BeamApplication(const char* signature)
	:
	BmApplication(signature, false),
	fInitCheck(B_NO_INIT),
	fMailWindow(NULL),
	fPrintSetup(NULL),
	fPrintJob("Mail"),
	fDeskbarItemIsOurs(false)
{
	gBeamApp = this;

	try {
		// create the GUI-info-roster:
		BeamGuiRoster = new BmGuiRoster();

		// load/determine all needed resources:
		BmResources::CreateInstance();
		TheResources->InitializeWithPrefs();

		ColumnListView::SetExtendedSelectionPolicy(
			ThePrefs->GetBool("ListviewLikeTracker", false));
				// make sure this actually get's initialized...

		// create BubbleHelper:
		BubbleHelper::CreateInstance();
		BmBusyView::SetErrorIcon(TheResources->IconByName("Error"));

		// init charset-tables:
		BmEncoding::InitCharsetMap();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "setting up foreign-keys" B_UTF8_ELLIPSIS));
		// now setup all foreign-key connections between these list-models:
		TheRecvAccountList->AddForeignKey(BmIdentity::MSG_RECV_ACCOUNT,
			TheIdentityList.Get());
		TheSmtpAccountList->AddForeignKey(BmIdentity::MSG_SMTP_ACCOUNT,
			TheIdentityList.Get());
		TheSignatureList->AddForeignKey(BmIdentity::MSG_SIGNATURE_NAME,
			TheIdentityList.Get());
		TheFilterChainList->AddForeignKey(BmRecvAccount::MSG_FILTER_CHAIN,
			TheRecvAccountList.Get());
		TheRecvAccountList->AddForeignKey(BmSmtpAccount::MSG_ACC_FOR_SAP,
			TheSmtpAccountList.Get());
		TheMailFolderList->AddForeignKey(BmRecvAccount::MSG_HOME_FOLDER,
			TheRecvAccountList.Get());
		TheMailFolderList->AddForeignKey(BmFilterAddon::FK_FOLDER,
			TheFilterList.Get());
		TheIdentityList->AddForeignKey(BmFilterAddon::FK_IDENTITY,
			TheFilterList.Get());

		// create the node-monitor looper and the stored action flusher:
		BmMailMonitor::CreateInstance();
		BmStoredActionFlusher::CreateInstance();

		// create the job status window:
		BmJobStatusWin::CreateInstance();
		TheJobStatusWin->Hide();
		TheJobStatusWin->Show();
		TheJobMetaController = TheJobStatusWin;

		BmPeopleMonitor::CreateInstance();
		BmPeopleList::CreateInstance();

		bm_plain_font = *be_plain_font;
		bm_bold_font = *be_bold_font;
		if (ThePrefs->GetBool("ListviewUsesStringSpacing")) {
			bm_plain_font.SetSpacing(B_STRING_SPACING);
			bm_bold_font.SetSpacing(B_STRING_SPACING);
		}

		add_system_beep_event(BM_BEEP_EVENT);

		BM_LOG(BM_LogApp,
			BmString(B_UTF8_ELLIPSIS "creating main-window" B_UTF8_ELLIPSIS));
		BmMainWindow::CreateInstance();

		TheBubbleHelper->EnableHelp(ThePrefs->GetBool("ShowTooltips", true));

		fStartupLocker->Unlock();
		BM_LOG(BM_LogApp, BmString("BeamApp-initialization done."));
	} catch (BM_error& error) {
		BM_SHOWERR(error.what());
		exit(10);
	}
}


/*----------------------------------------------------------------------------*\
	~BeamApplication()
		-	standard destructor
\*----------------------------------------------------------------------------*/
BeamApplication::~BeamApplication()
{
	RemoveDeskbarItem();
	ThePeopleMonitor = NULL;
	TheStoredActionFlusher = NULL;
	TheMailMonitor = NULL;
	ThePeopleList = NULL;
	delete fPrintSetup;
	delete TheResources;
	delete BeamGuiRoster;
}

/*----------------------------------------------------------------------------*\
	ReadyToRun()
		-	ensures that main-window is visible
		-	if Beam has been instructed to show a specific mail on startup, the
			mail-window is shown on top of the main-window
\*----------------------------------------------------------------------------*/
void
BeamApplication::ReadyToRun()
{
	if (TheMainWindow->IsMinimized())
		TheMainWindow->Minimize(false);

	if (fMailWindow != NULL) {
		TheMainWindow->SendBehind(fMailWindow);
		fMailWindow = NULL;
	}
}


/*------------------------------------------------------------------------------*\
	Run()
		-	starts Beam
\*------------------------------------------------------------------------------*/
thread_id
BeamApplication::Run()
{
	if (InitCheck() != B_OK) {
		exit(10);
	}

	thread_id threadId = 0;
	try {
		if (BeamInTestMode) {
			// in test-mode the main-window will only be shown when neccessary:
			TheMainWindow->Hide();
			// Now wait until Test-thread allows us to start...
			snooze(200 * 1000);
			fStartupLocker->Lock();
		} else if (ThePrefs->GetBool( "UseDeskbar"))
			InstallDeskbarItem();

		// start most of our list-models:
		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading receving accounts" B_UTF8_ELLIPSIS));
		TheRecvAccountList->StartJobInNewThread();

		// start most of our list-models:
		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading identities" B_UTF8_ELLIPSIS));
		TheIdentityList->StartJobInThisThread();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading signatures" B_UTF8_ELLIPSIS));
		TheSignatureList->StartJobInThisThread();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading filters" B_UTF8_ELLIPSIS));
		TheFilterList->StartJobInNewThread();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading filter chains" B_UTF8_ELLIPSIS));
		TheFilterChainList->StartJobInNewThread();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "reading SMTP accounts" B_UTF8_ELLIPSIS));
		TheSmtpAccountList->StartJobInThisThread();

		BM_LOG(BM_LogApp, BmString(
			B_UTF8_ELLIPSIS "querying people" B_UTF8_ELLIPSIS));
		ThePeopleList->StartJobInNewThread();

		BM_LOG(BM_LogApp, BmString("Showing main window."));
		TheMainWindow->Show();

		threadId = BmApplication::Run();

		ThePrefs->Store();
			// always store prefs since it contains references to
			// list-items that are tracked via foreign-keys. If the
			// user has renamed a folder, for instance the references
			// to this folder will have moved along automatically, but
			// we need to store these new prefs, as otherwise it would
			// be lost from the next session onwards.
		TheRecvAccountList->StoreIfNeeded();
			// store recv-account-list since the certificate info may have
			// changed
		TheSmtpAccountList->StoreIfNeeded();
			// store smtp-account-list since the certificate info may have
			// changed
		TheIdentityList->StoreIfNeeded();
			// store identity-list since the current identity may have changed
		ThePeopleList->StoreIfNeeded();
			// store people-list since new known addresses may have been added
	} catch(BM_error& error) {
		BM_SHOWERR(error.what());
		exit(10);
	}

	return threadId;
}


void
BeamApplication::InstallDeskbarItem()
{
	if (fDeskbar.HasItem(BM_DeskbarItemName))
		return;

	app_info appInfo;
	status_t result = gBeamApp->GetAppInfo(&appInfo);
	if (result != B_OK)
		return;

	int32 id;
	appInfo.ref.set_name(BM_DeskbarItemName);
	result = fDeskbar.AddItem(&appInfo.ref, &id);
	if (result != B_OK) {
		BM_SHOWERR(BmString("Unable to install Beam_DeskbarItem (")
			<< BM_DeskbarItemName << ").\nError: \n\t" << strerror(result));
	} else
		fDeskbarItemIsOurs = true;
}


void
BeamApplication::RemoveDeskbarItem()
{
	if (fDeskbarItemIsOurs && fDeskbar.HasItem(BM_DeskbarItemName))
		fDeskbar.RemoveItem(BM_DeskbarItemName);
}


bool
BeamApplication::QuitRequested()
{
	BM_LOG(BM_LogApp, "App: quit requested, checking state" B_UTF8_ELLIPSIS);
	fIsQuitting = true;
	bool shouldQuit = true;

	if (TheMailMonitor->LockLooper()) {
		int32 count = CountWindows();
		// first check if there are any active jobs:
		if (TheJobStatusWin && TheJobStatusWin->HasActiveJobs()) {
			BAlert* alert = new BAlert("Active jobs",
				"There are still some jobs/connections active!"
					"\nDo you really want to quit now?",
				"Quit", "Cancel", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(1, B_ESCAPE);
			if (alert->Go() == 1)
				shouldQuit = false;
		}

		if (shouldQuit) {
			// ask all windows if they are ready to quit, in which case we
			// actually do quit (only if *ALL* windows indicate that they are
			// prepared to quit!):
			for (uint32 index = count - 1; shouldQuit && index >= 0; --index) {
				BWindow* window = gBeamApp->WindowAt(index);
				if (window == NULL)
					continue;

				window->Lock();
				if (!window->QuitRequested())
					shouldQuit = false;
				window->Unlock();
			}
		}

		if (!shouldQuit) {
			TheMailMonitor->UnlockLooper();
			fIsQuitting = false;
		} else {
			TheStoredActionFlusher->Quit();
			TheMailMonitor->Quit();
			for (uint32 index = count - 1; index >= 0; --index) {
				BWindow* window = gBeamApp->WindowAt(index);
				if (window != NULL) {
					window->LockLooper();
					window->Quit();
				}
			}
		}
	}

	snooze(200 * 1000);
		// there might be slaves running, give them some more time to stop.

	BM_LOG(BM_LogApp, shouldQuit ? "ok, app is quitting"
		: "no, app isn't quitting");
	return shouldQuit;
}


/*----------------------------------------------------------------------------*\
	ArgvReceived(argc, argv)
		-	first argument is interpreted to be a destination mail-address, so a
			new mail is generated for if an argument has been provided
\*----------------------------------------------------------------------------*/
void
BeamApplication::ArgvReceived(int32 argc, char** argv)
{
	if (argc > 1 && !BeamInTestMode) {
		BmString arg(argv[1]);
		if (arg.ICompare("mailto:", 7) == 0)
			LaunchURL(arg);
		else {
			BMessage message(BMM_NEW_MAIL);
			if (arg.ICompare("enclosure:", 10) == 0)
				message.AddString(MSG_ENCLOSE, arg.String() + 10);
			else
				message.AddString(MSG_WHO_TO, arg.String());

			PostMessage(&message);
		}
	}
}


/*----------------------------------------------------------------------------*\
	RefsReceived( message)
		-	every given reference is opened, draft and pending messages are
			shown in the mail-edit-window, while other mails are opened
			view-only.
\*----------------------------------------------------------------------------*/
void
BeamApplication::RefsReceived(BMessage* message)
{
	if (message == NULL)
		return;

	entry_ref inputRef;
	entry_ref entryRef;
	BEntry entry;
	for (int index = 0; message->FindRef("refs", index, &inputRef) == B_OK;
		++index) {
		if (entry.SetTo(&inputRef, true) != B_OK
			|| entry.GetRef(&entryRef) != B_OK)
			continue;

		BmRef<BmMailRef> ref = BmMailRef::CreateInstance(entryRef);
		if (ref && ref->IsValid()) {
			if (ref->Status().Length() == 0) {
				// mail has no status, meaning that it never has been
				// stored properly (the attributes haven't been written).
				// We read the mail and (re-)store it with the attributes:
				BmRef<BmMail> mail = BmMail::CreateInstance(ref.Get());
				mail->StartJobInThisThread(BmMail::BM_READ_MAIL_JOB);
				if (!mail->IsJobCompleted()) {
					BM_LOGERR(BmString("Unable to load mail ")
						<< entryRef.name);
					continue;
				}
				mail->Store();
				ref = mail->MailRef();
					// update mail-ref with the attributes just created
			}

			if (ref->Status() == BM_MAIL_STATUS_DRAFT
				|| ref->Status() == BM_MAIL_STATUS_PENDING) {
				BmMailEditWin* editWindow =
					BmMailEditWin::CreateInstance(ref.Get());
				if (editWindow != NULL) {
					editWindow->Show();
					if (fMailWindow == NULL)
						fMailWindow = editWindow;
				}
			} else {
				BmMailViewWin* viewWindow =
					BmMailViewWin::CreateInstance(ref.Get());
				if (viewWindow != NULL) {
					viewWindow->Show();
					if (fMailWindow == NULL)
						fMailWindow = viewWindow;
				}
			}
		} else if (CheckMimeType(&entryRef, "application/x-person")) {
			BmStringVect emails;
			ThePeopleList->GetEmailsFromPeopleFile(entryRef, emails);
			BmString email = SelectEmailForPerson(emails);
			BmRef<BmMail> mail = new BmMail(true);
			mail->SetFieldVal(BM_FIELD_TO, email);
			BmMailEditWin* editWindow =
				BmMailEditWin::CreateInstance(mail.Get());
			if (editWindow != NULL)
				editWindow->Show();
		}
	}
}


/*----------------------------------------------------------------------------*\
	MessageReceived( message)
		-	handles all actions on mailrefs, like replying, forwarding,
			and creating new mails
\*----------------------------------------------------------------------------*/
void
BeamApplication::MessageReceived(BMessage* message)
{
	try {
		switch(message->what) {
			case BM_JOBWIN_POP:
			case BM_JOBWIN_IMAP:
			case BMM_CHECK_MAIL:
			{
				while (TheRecvAccountList->IsJobRunning())
					snooze(200 * 1000);

				const char* key = NULL;
				message->FindString(BmRecvAccountList::MSG_ITEMKEY, &key);
				if (key != NULL) {
					bool isAutoCheck =
						message->FindBool(BmRecvAccountList::MSG_AUTOCHECK);
					BM_LOG(BM_LogApp, BmString("RecvAccount ") << key
						<< " asks to check mail "
						<< (isAutoCheck ? "(auto)" : "(manual)"));
					if (!isAutoCheck
						|| !ThePrefs->GetBool("AutoCheckOnlyIfPPPRunning", true)
						|| IsPPPRunning()) {
						BM_LOG(BM_LogApp, BmString("RecvAccount ") << key
							<< ": mail is checked now");
						TheRecvAccountList->CheckMailFor(key, isAutoCheck);
						if (!isAutoCheck
						&& ThePrefs->GetBool("SendPendingMailsOnCheck", true))
							TheSmtpAccountList->SendPendingMails();
					} else
						BM_LOG(BM_LogApp, BmString("RecvAccount ") << key
							<< ": mail is not checked (PPP isn't running)");
				} else {
					TheRecvAccountList->CheckMail(false);
					if (ThePrefs->GetBool("SendPendingMailsOnCheck", true))
						TheSmtpAccountList->SendPendingMails();
				}
				break;
			}

			case BMM_CHECK_ALL:
			{
				BM_LOG(BM_LogApp, "Request to check mail for all accounts");
				while(TheRecvAccountList->IsJobRunning())
					snooze(200 * 1000);

				TheRecvAccountList->CheckMail(true);
				if (ThePrefs->GetBool("SendPendingMailsOnCheck", true))
					TheSmtpAccountList->SendPendingMails();

				break;
			}

			case BMM_NEW_MAIL:
			{
				BmRef<BmMail> mail = new BmMail(true);
				const char* to = message->FindString(MSG_WHO_TO);
				if (to != NULL)
					mail->SetFieldVal(BM_FIELD_TO, to);

				const char* optField = NULL;
				const char* enclPath = NULL;
				int32 index;
				for (index = 0;
					message->FindString(MSG_OPT_FIELD, index, &optField)
						== B_OK;
					++index) {
					mail->SetFieldVal(optField, optField);
				}
				BM_LOG(BM_LogApp, BmString("Asked to create new mail with ")
					<< index << " options");

				BmMailEditWin* editWindow =
					BmMailEditWin::CreateInstance(mail.Get());
				if (editWindow != NULL)
					editWindow->Show();

				BEntry entry;
				entry_ref entryRef;
				status_t result;
				for (index = 0; message->FindString(MSG_ENCLOSE, index,
					&enclPath) == B_OK; ++index) {
					entry.SetTo(enclPath, true);
					result = entry.GetRef(&entryRef);
					if (result == B_OK) {
						BMessage* attachMsg = new BMessage(BMM_ATTACH);
						attachMsg->AddRef("refs", &entryRef);
						editWindow->PostMessage(attachMsg);
					}
				}

				break;
			}

			case BMM_REDIRECT:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Redirector", RedirectMails, message);
				break;
			}

			case BMM_EDIT_AS_NEW:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Editor", EditMailsAsNew, message);
				break;
			}

			case BMM_REPLY:
			case BMM_REPLY_LIST:
			case BMM_REPLY_ORIGINATOR:
			case BMM_REPLY_ALL:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Replier", ReplyToMails, message);
				break;
			}

			case BMM_FORWARD_ATTACHED:
			case BMM_FORWARD_INLINE:
			case BMM_FORWARD_INLINE_ATTACH:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Forwarder", ForwardMails, message);
				break;
			}

			case BMM_MARK_AS:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Marker", MarkMailsAs, message);
				break;
			}

			case BMM_MOVE:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Mover", MoveMails, message);
				break;
			}

			case c_about_window_url_invoked:
			{
				const char* url;
				if (message->FindString("url", &url) == B_OK)
					LaunchURL(url);

				break;
			}

			case BMM_PAGE_SETUP:
				PageSetup();
				break;

			case BMM_PRINT:
			{
				if (fPrintSetup == NULL)
					PageSetup();
				if (fPrintSetup != NULL) {
					DetachCurrentMessage();
					sSlaveHandler.Run("Msg-Printer", PrintMails, message, 2);
				}

				break;
			}

			case BMM_PREFERENCES:
			{
				if (ThePrefsWin == NULL) {
					BmPrefsWin::CreateInstance();
					ThePrefsWin->Show();
				} else {
					if (ThePrefsWin->LockLooper()) {
						ThePrefsWin->Hide();
						ThePrefsWin->Show();
						ThePrefsWin->UnlockLooper();
					}
				}

				BmString subViewName = message->FindString("SubViewName");
				if (subViewName.Length())
					ThePrefsWin->PostMessage(message);

				break;
			}

			case BMM_TRASH:
			{
				DetachCurrentMessage();
				sSlaveHandler.Run("Msg-Trasher", TrashMails, message);
				break;
			}

			case B_SILENT_RELAUNCH:
			{
				BM_LOG(BM_LogApp, "App: silently relaunched");
				if (TheMainWindow->IsMinimized())
					TheMainWindow->Minimize(false);
				BmApplication::MessageReceived(message);

				break;
			}

			case BM_DESKBAR_GET_MBOX:
			{
				if (!message->IsReply()) {
					BMessage reply(BM_DESKBAR_GET_MBOX);
					entry_ref mboxRef;
					BEntry entry(ThePrefs->GetString("MailboxPath").String(),
						true);
					if (entry.GetRef(&mboxRef) == B_OK) {
						reply.AddRef("mbox", &mboxRef);
						message->SendReply(&reply, NULL, 1000000);
					}
				}

				break;
			}

			case BMM_CREATE_PERSON_FROM_ADDR:
			{
				const char* name = NULL;
				const char* email = NULL;
				bool edit = false;
				message->FindBool("edit", &edit);

				entry_ref entryRef;
				for (uint32 index = 0; message->FindString("name", index, &name)
					== B_OK; index++) {
					message->FindString("email", index, &email);
					BmRef<BmPerson> person = ThePeopleList->FindPersonByName(
						name);
					if (person) {
						entryRef = person->EntryRef();
						person->CreateNewEmail(email);
					} else
						ThePeopleList->CreateNewPerson(name, email, &entryRef);

					if (edit)
						be_roster->Launch(&entryRef);
				}

				break;
			}

			case BMM_EDIT_PERSON_WITH_ADDR:
			{
				const char* email = NULL;
				entry_ref entryRef;
				for (uint32 index = 0; message->FindString("email", index,
					&email) == B_OK; index++) {
					BmRef<BmPerson> person = ThePeopleList->FindPersonByEmail(
						email);
					if (person) {
						entryRef = person->EntryRef();
						be_roster->Launch(&entryRef);
					}
				}

				break;
			}

			default:
				BmApplication::MessageReceived(message);
				break;
		}
	} catch(BM_error& error) {
		// a problem occurred, we tell the user:
		BM_SHOWERR(BmString("BmApp: ") << error.what());
	}
}


/*----------------------------------------------------------------------------*\
	PageSetup()
		-	sets up the basic printing environment
\*----------------------------------------------------------------------------*/
void
BeamApplication::PageSetup()
{
	if (fPrintSetup != NULL)
		fPrintJob.SetSettings(new BMessage(*fPrintSetup));

	status_t result = fPrintJob.ConfigPage();
	if (result == B_OK) {
		delete fPrintSetup;
		fPrintSetup = fPrintJob.Settings();
	}
}


/*----------------------------------------------------------------------------*\
	LaunchURL(url)
		-	launches the corresponding program for the given URL
			(usually WebPositive)
		-	mailto: - URLs are handled internally
\*----------------------------------------------------------------------------*/
void
BeamApplication::LaunchURL(const BmString url)
{
	char* urlStr = const_cast<char*>(url.String());
	status_t result;
	if (url.ICompare("https:", 6) == 0)
		result = be_roster->Launch("application/x-vnd.Be.URL.https", 1,
			&urlStr);
	else if (url.ICompare("http:", 5) == 0)
		result = be_roster->Launch("application/x-vnd.Be.URL.http", 1, &urlStr);
	else if (url.ICompare("ftp:", 4) == 0)
		result = be_roster->Launch("application/x-vnd.Be.URL.ftp", 1, &urlStr);
	else if (url.ICompare("file:", 5) == 0)
		result = be_roster->Launch("application/x-vnd.Be.URL.file", 1, &urlStr);
	else if (url.ICompare("mailto:", 7) == 0) {
		BMessage message(BMM_NEW_MAIL);
		BmString to(urlStr + 7);
		int32 optPos = to.IFindFirst("?");
		if (optPos != B_ERROR) {
			BmString opts;
			to.MoveInto(opts, optPos, to.Length());
			regexx::Regexx rx;
			int32 optCount = rx.exec(opts, "[?&]([^&=]+)=([^&]+)",
				regexx::Regexx::global);
			for (int32 index = 0; index < optCount; ++index) {
				BmString rawVal(rx.match[index].atom[1]);
				rawVal.DeUrlify();
				BmString field(rx.match[index].atom[0]);
				message.AddString(MSG_OPT_FIELD, field.String());
				message.AddString(MSG_OPT_VALUE, rawVal.String());
			}
		}
		to.DeUrlify();
		message.AddString( BeamApplication::MSG_WHO_TO, to.String());
		gBeamApp->PostMessage(&message);

		return;
	} else
		result = B_ERROR;

	if (!(result == B_OK || result == B_ALREADY_RUNNING)) {
		BAlert alert = new BAlert("", (BmString(
			"Could not launch application for url\n\t") << url
				<< "\n\nError:\n\t" << strerror(result)).String(),
			"OK");
		alert->Go();
	}
}


void
BeamApplication::AboutRequested()
{
	ImageAboutWindow* aboutWindow = new ImageAboutWindow("About Beam", "Beam",
		TheResources->IconByName("AboutIcon")->bitmap, 15,
		"BEware, Another Mailer\n(c) Oliver Tappe, Berlin, Germany", "",
		"https://github.com/HaikuArchives/Beam",
		"\n\n\n\n\n\n\nBeam makes use of the following Software:\n\
\n\
	Libiconv, by Bruno Haible\n\
\n\
	Liblayout, by Marco Nelissen\n\
\n\
	OSBF from crm114,\n\
	    by Bill Yerazunis (crm114)\n\
	    and Fidelis Assis (OSBF)\n\
\n\
	PCRE, by Philip Hazel\n\
\n\
	Regexx, by Gustavo Niemeyer\n\
\n\
	SantasGiftBag, by Brian Tietz\n\
\n\
	if available:\n\
	    openssl, by the OpenSSL Project\n\
\n\n\n\n\n\n\nThanks to:\n\
\n\
Heike Herfart \n\
	for understanding the geek, \n\
	the testing sessions \n\
	and many, many suggestions. \n\
\n\
\n\
...and (in alphabetical order):\n\
\n\
Adam McNutt\n\
Alan Westbrook\n\
Atillâ Öztürk\n\
Bernd Thorsten Korz\n\
Cedric Vincent\n\
Charlie Clark\n\
David Vignoni\n\
Eberhard Hafermalz\n\
Eugenia Loli-Queru\n\
Hartmut Reh\n\
Helmar Rudolph\n\
Ingo Weinhold\n\
Jace Cavacini\n\
Jens Neuwerk\n\
Jon Hart\n\
Kevin Musick\n\
Koki\n\
Lars Müller\n\
Len G. Jacob\n\
Linus Almstrom\n\
Mathias Reitinger\n\
Max Hartmann\n\
MDR-team (MailDaemonReplacement)\n\
Mikael Larsson\n\
Mikhail Panasyuk\n\
Olivier Milla\n\
qwilk\n\
Paweł Lewicki\n\
Rainer Riedl\n\
Rob Lund\n\
Shard\n\
Stephan Assmus\n\
Stephan Buelling\n\
Stephen Butters\n\
Tyler Dauwalder\n\
Zach\n\
\n\
\n\n\n\n\
...and thanks to everyone I forgot, too!\n\
\n\
\n\n\n\n\n\n"
	);
	aboutWindow->Show();
}


/*----------------------------------------------------------------------------*\
	ScreenFrame()
		-	returns the the current screen's frame
\*----------------------------------------------------------------------------*/
BRect
BeamApplication::ScreenFrame() const
{
	BScreen screen(TheMainWindow);
	if (!screen.IsValid())
		BM_SHOWERR(BmString("Could not initialize BScreen object !?!"));

	return screen.Frame();
}


/*----------------------------------------------------------------------------*\
	SetNewWorkspace( newWorkspace)
		-	ensures that main-window and job-status-window are always shown
			inside the same workspace
\*----------------------------------------------------------------------------*/
void
BeamApplication::SetNewWorkspace(uint32 newWorkspace)
{
	if (TheMainWindow->Workspaces() != newWorkspace)
		TheMainWindow->SetWorkspaces(newWorkspace);
	if (TheJobStatusWin->Workspaces() != newWorkspace)
		TheJobStatusWin->SetWorkspaces(newWorkspace);
}


uint32
BeamApplication::CurrentWorkspace() const
{
	return TheMainWindow->Workspaces();
}


/*----------------------------------------------------------------------------*\
	HandlesMimetype(mimetype)
		-	determines whether or not Beam handles the given mimetype
		-	returns true for any mail-related types, false otherwise
\*----------------------------------------------------------------------------*/
bool
BeamApplication::HandlesMimetype(const BmString mimetype) const
{
	return BeamRoster->IsSupportedEmailMimeType(mimetype);
}
