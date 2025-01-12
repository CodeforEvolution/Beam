/*
 * Copyright 2002-2025, project beam (http://sourceforge.net/projects/beam).
 * All rights reserved. Distributed under the terms of the GNU GPL v2.
 *
 * Authors:
 *		Oliver Tappe <beam@hirschkaefer.de>
 */
#ifndef _BEAM_APP_H
#define _BEAM_APP_H


#include <Deskbar.h>
#include <PrintJob.h>
#include <Rect.h>

#include "BmString.h"

#include "BmApp.h"


class BLocker;
class BView;
class BmWindow;

extern const char* BM_APP_SIG;

enum {
	BMM_SET_BUSY				= 'bMxa',
	BMM_UNSET_BUSY				= 'bMxb',
	BMM_CREATE_PERSON_FROM_ADDR	= 'bMxc',
	BMM_EDIT_PERSON_WITH_ADDR	= 'bMxd'
};


class BeamApplication : public BmApplication {
	friend	int32				PrintMails(void* data);

public:
								BeamApplication(const char* signature);
	virtual						~BeamApplication();

	// Native methods
			BRect				ScreenFrame() const;
			void				SetNewWorkspace(uint32 newWorkspace);
			uint32				CurrentWorkspace() const;

			bool				HandlesMimetype(const BmString mimetype) const;
			void				LaunchURL(const BmString url);

	// BApplication methods
	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();
	virtual	void				AboutRequested();
	virtual	void				ReadyToRun();
	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	void				RefsReceived(BMessage* message);
	virtual	thread_id			Run();

	// Message-fields
	static	const char* const	MSG_MAILREF_VECT;
	static	const char* const	MSG_STATUS;
	static	const char* const	MSG_WHO_TO;
	static	const char* const	MSG_OPT_FIELD;
	static	const char* const	MSG_OPT_VALUE;
	static	const char* const	MSG_SUBJECT;
	static	const char* const	MSG_SELECTED_TEXT;
	static	const char* const	MSG_SENDING_REFVIEW;
	static	const char* const	MSG_ENCLOSE;

private:
			void				PageSetup();

			void				InstallDeskbarItem();
			void				RemoveDeskbarItem();

private:
			status_t			fInitCheck;
			BmWindow*			fMailWindow;

			BDeskbar			fDeskbar;

			BMessage*			fPrintSetup;
			BPrintJob			fPrintJob;

			bool				fDeskbarItemIsOurs;
};

extern BeamApplication* gBeamApp;

#endif // _BEAM_APP_H
