// @(#)root/proof:$Id$
// Author: Fons Rademakers   13/02/97

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TProof                                                               //
//                                                                      //
// This class controls a Parallel ROOT Facility, PROOF, cluster.        //
// It fires the worker servers, it keeps track of how many workers are  //
// running, it keeps track of the workers running status, it broadcasts //
// messages to all workers, it collects results, etc.                   //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#ifdef WIN32
#   include <io.h>
#   include <sys/stat.h>
#   include <sys/types.h>
#else
#   include <unistd.h>
#endif
#include <vector>

#include "RConfigure.h"
#include "Riostream.h"
#include "Getline.h"
#include "TBrowser.h"
#include "TChain.h"
#include "TCondor.h"
#include "TDSet.h"
#include "TError.h"
#include "TEnv.h"
#include "TEventList.h"
#include "TFile.h"
#include "TFileInfo.h"
#include "TFTP.h"
#include "THashList.h"
#include "TInterpreter.h"
#include "TKey.h"
#include "TMap.h"
#include "TMessage.h"
#include "TMonitor.h"
#include "TMutex.h"
#include "TObjArray.h"
#include "TObjString.h"
#include "TParameter.h"
#include "TProof.h"
#include "TProofNodeInfo.h"
#include "TVirtualProofPlayer.h"
#include "TVirtualPacketizer.h"
#include "TProofServ.h"
#include "TPluginManager.h"
#include "TQueryResult.h"
#include "TRandom.h"
#include "TRegexp.h"
#include "TROOT.h"
#include "TSemaphore.h"
#include "TSlave.h"
#include "TSocket.h"
#include "TSortedList.h"
#include "TSystem.h"
#include "TThread.h"
#include "TTree.h"
#include "TUrl.h"
#include "TFileCollection.h"
#include "TProofDataSetManager.h"

TProof *gProof = 0;
TVirtualMutex *gProofMutex = 0;

TList   *TProof::fgProofEnvList = 0;  // List of env vars for proofserv

ClassImp(TProof)

//----- PROOF Interrupt signal handler -----------------------------------------
//______________________________________________________________________________
Bool_t TProofInterruptHandler::Notify()
{
   // TProof interrupt handler.

   Info("Notify","Processing interrupt signal ...");

   // Stop any remote processing
   fProof->StopProcess(kTRUE);

   // Handle also interrupt condition on socket(s)
   fProof->Interrupt(TProof::kLocalInterrupt);

   return kTRUE;
}

//----- Input handler for messages from TProofServ -----------------------------
//______________________________________________________________________________
TProofInputHandler::TProofInputHandler(TProof *p, TSocket *s)
                   : TFileHandler(s->GetDescriptor(),1),
                     fSocket(s), fProof(p)
{
   // Constructor
}

//______________________________________________________________________________
Bool_t TProofInputHandler::Notify()
{
   // Handle input

   fProof->CollectInputFrom(fSocket);
   return kTRUE;
}


//------------------------------------------------------------------------------

ClassImp(TSlaveInfo)

//______________________________________________________________________________
Int_t TSlaveInfo::Compare(const TObject *obj) const
{
   // Used to sort slaveinfos by ordinal.

   if (!obj) return 1;

   const TSlaveInfo *si = dynamic_cast<const TSlaveInfo*>(obj);

   if (!si) return fOrdinal.CompareTo(obj->GetName());

   const char *myord = GetOrdinal();
   const char *otherord = si->GetOrdinal();
   while (myord && otherord) {
      Int_t myval = atoi(myord);
      Int_t otherval = atoi(otherord);
      if (myval < otherval) return 1;
      if (myval > otherval) return -1;
      myord = strchr(myord, '.');
      if (myord) myord++;
      otherord = strchr(otherord, '.');
      if (otherord) otherord++;
   }
   if (myord) return -1;
   if (otherord) return 1;
   return 0;
}

//______________________________________________________________________________
void TSlaveInfo::Print(Option_t *opt) const
{
   // Print slave info. If opt = "active" print only the active
   // slaves, if opt="notactive" print only the not active slaves,
   // if opt = "bad" print only the bad slaves, else
   // print all slaves.

   TString stat = fStatus == kActive ? "active" :
                  fStatus == kBad ? "bad" :
                  "not active";
   TString msd  = fMsd.IsNull() ? "<null>" : fMsd.Data();

   if (!opt) opt = "";
   if (!strcmp(opt, "active") && fStatus != kActive)
      return;
   if (!strcmp(opt, "notactive") && fStatus != kNotActive)
      return;
   if (!strcmp(opt, "bad") && fStatus != kBad)
      return;

   cout << "Slave: "          << fOrdinal
        << "  hostname: "     << fHostName
        << "  msd: "          << msd
        << "  perf index: "   << fPerfIndex
        << "  "               << stat
        << endl;
}


//------------------------------------------------------------------------------

//______________________________________________________________________________
static char *CollapseSlashesInPath(const char *path)
{
   // Get rid of spare slashes in a path. Returned path must be deleted[]
   // by the user.

   if (path) {
      Int_t i = 1; // current index as we go along the string
      Int_t j = 0; // current end of new path in newPath
      char *newPath = new char [strlen(path) + 1];
      newPath[0] = path[0];
      while (path[i]) {
         if (path[i] != '/' || newPath[j] != '/') {
            j++;
            newPath[j] = path[i];
         }
         i++;
      }
      if (newPath[j] != '/')
         j++;
      newPath[j] = 0; // We have to terminate the new path.
      return newPath;
   }
   return 0;
}

ClassImp(TProof)

TSemaphore    *TProof::fgSemaphore = 0;

//______________________________________________________________________________
TProof::TProof(const char *masterurl, const char *conffile, const char *confdir,
               Int_t loglevel, const char *alias, TProofMgr *mgr)
       : fUrl(masterurl)
{
   // Create a PROOF environment. Starting PROOF involves either connecting
   // to a master server, which in turn will start a set of slave servers, or
   // directly starting as master server (if master = ""). Masterurl is of
   // the form: [proof[s]://]host[:port]. Conffile is the name of the config
   // file describing the remote PROOF cluster (this argument alows you to
   // describe different cluster configurations).
   // The default is proof.conf. Confdir is the directory where the config
   // file and other PROOF related files are (like motd and noproof files).
   // Loglevel is the log level (default = 1). User specified custom config
   // files will be first looked for in $HOME/.conffile.

   // Synchronize closing with actions like MarkBad
   fCloseMutex = 0;

   // This may be needed during init
   fManager = mgr;

   // Default server type
   fServType = TProofMgr::kXProofd;

   // Default query mode
   fQueryMode = kSync;

   // Parse the main URL, adjusting the missing fields and setting the relevant
   // bits
   ResetBit(TProof::kIsClient);
   ResetBit(TProof::kIsMaster);

   // Protocol and Host
   if (!masterurl || strlen(masterurl) <= 0) {
      fUrl.SetProtocol("proof");
      fUrl.SetHost("__master__");
   } else if (!(strstr(masterurl, "://"))) {
      fUrl.SetProtocol("proof");
   }

   // Port
   if (fUrl.GetPort() == TUrl(" ").GetPort())
      fUrl.SetPort(TUrl("proof:// ").GetPort());

   // User
   if (strlen(fUrl.GetUser()) <= 0) {
      // Get user logon name
      UserGroup_t *pw = gSystem->GetUserInfo();
      if (pw) {
         fUrl.SetUser(pw->fUser);
         delete pw;
      }
   }

   // Make sure to store the FQDN, so to get a solid reference for subsequent checks
   if (!strcmp(fUrl.GetHost(), "__master__"))
      fMaster = fUrl.GetHost();
   else if (!strlen(fUrl.GetHost()))
      fMaster = gSystem->GetHostByName(gSystem->HostName()).GetHostName();
   else
      fMaster = gSystem->GetHostByName(fUrl.GetHost()).GetHostName();

   // Server type
   if (strlen(fUrl.GetOptions()) > 0) {
      if (!(strncmp(fUrl.GetOptions(),"std",3))) {
         fServType = TProofMgr::kProofd;
      } else if (!(strncmp(fUrl.GetOptions(),"lite",4))) {
         fServType = TProofMgr::kProofLite;
      }
      fUrl.SetOptions("");
   }

   // Instance type
   fMasterServ = kFALSE;
   SetBit(TProof::kIsClient);
   ResetBit(TProof::kIsMaster);
   if (fMaster == "__master__") {
      fMasterServ = kTRUE;
      ResetBit(TProof::kIsClient);
      SetBit(TProof::kIsMaster);
   } else if (fMaster == "prooflite") {
      // Client and master are merged
      fMasterServ = kTRUE;
      SetBit(TProof::kIsMaster);
   }

   Init(masterurl, conffile, confdir, loglevel, alias);

   // If called by a manager, make sure it stays in last position
   // for cleaning
   if (mgr) {
      R__LOCKGUARD2(gROOTMutex);
      gROOT->GetListOfSockets()->Remove(mgr);
      gROOT->GetListOfSockets()->Add(mgr);
   }

   // Old-style server type: we add this to the list and set the global pointer
   if (IsProofd() || TestBit(TProof::kIsMaster))
      gROOT->GetListOfProofs()->Add(this);

   // Still needed by the packetizers: needs to be changed
   gProof = this;
}

//______________________________________________________________________________
TProof::TProof() : fUrl(""), fServType(TProofMgr::kXProofd)
{
   // Protected constructor to be used by classes deriving from TProof
   // (they have to call Init themselves and override StartSlaves
   // appropriately).
   //
   // This constructor simply closes any previous gProof and sets gProof
   // to this instance.

   fValid = kFALSE;
   fRecvMessages = 0;
   fSlaveInfo = 0;
   fMasterServ = kFALSE;
   fSendGroupView = kFALSE;
   fActiveSlaves = 0;
   fInactiveSlaves = 0;
   fUniqueSlaves = 0;
   fAllUniqueSlaves = 0;
   fNonUniqueMasters = 0;
   fActiveMonitor = 0;
   fUniqueMonitor = 0;
   fAllUniqueMonitor = 0;
   fCurrentMonitor = 0;
   fBytesRead = 0;
   fRealTime = 0;
   fCpuTime = 0;
   fIntHandler = 0;
   fProgressDialog = 0;
   fProgressDialogStarted = kFALSE;
   fPlayer = 0;
   fFeedback = 0;
   fChains = 0;
   fDSet = 0;
   fNotIdle = 0;
   fSync = kTRUE;
   fRunStatus = kRunning;
   fIsWaiting = kFALSE;
   fRedirLog = kFALSE;
   fLogFileW = 0;
   fLogFileR = 0;
   fLogToWindowOnly = kFALSE;

   fWaitingSlaves = 0;
   fQueries = 0;
   fOtherQueries = 0;
   fDrawQueries = 0;
   fMaxDrawQueries = 1;
   fSeqNum = 0;

   fSessionID = -1;
   fEndMaster = kFALSE;

   fGlobalPackageDirList = 0;
   fPackageLock = 0;
   fEnabledPackagesOnClient = 0;

   fInputData = 0;

   fPrintProgress = 0;

   fLoadedMacros = 0;

   fProtocol = -1;
   fSlaves = 0;
   fBadSlaves = 0;
   fAllMonitor = 0;
   fDataReady = kFALSE;
   fBytesReady = 0;
   fTotalBytes = 0;
   fAvailablePackages = 0;
   fEnabledPackages = 0;
   fRunningDSets = 0;

   fCollectTimeout = -1;

   fManager = 0;
   fQueryMode = kSync;
   fDynamicStartup = kFALSE;

   fCloseMutex = 0;

   gROOT->GetListOfProofs()->Add(this);

   gProof = this;
}

//______________________________________________________________________________
TProof::~TProof()
{
   // Clean up PROOF environment.

   while (TChain *chain = dynamic_cast<TChain*> (fChains->First()) ) {
      // remove "chain" from list
      chain->SetProof(0);
      RemoveChain(chain);
   }

   // remove links to packages enabled on the client
   if (TestBit(TProof::kIsClient)) {
      // iterate over all packages
      TIter nextpackage(fEnabledPackagesOnClient);
      while (TObjString *package = dynamic_cast<TObjString*>(nextpackage())) {
         FileStat_t stat;
         gSystem->GetPathInfo(package->String(), stat);
         // check if symlink, if so unlink
         // NOTE: GetPathInfo() returns 1 in case of symlink that does not point to
         // existing file or to a directory, but if fIsLink is true the symlink exists
         if (stat.fIsLink)
            gSystem->Unlink(package->String());
      }
   }

   Close();
   SafeDelete(fIntHandler);
   SafeDelete(fSlaves);
   SafeDelete(fActiveSlaves);
   SafeDelete(fInactiveSlaves);
   SafeDelete(fUniqueSlaves);
   SafeDelete(fAllUniqueSlaves);
   SafeDelete(fNonUniqueMasters);
   SafeDelete(fBadSlaves);
   SafeDelete(fAllMonitor);
   SafeDelete(fActiveMonitor);
   SafeDelete(fUniqueMonitor);
   SafeDelete(fAllUniqueMonitor);
   SafeDelete(fSlaveInfo);
   SafeDelete(fChains);
   SafeDelete(fPlayer);
   SafeDelete(fFeedback);
   SafeDelete(fWaitingSlaves);
   SafeDelete(fAvailablePackages);
   SafeDelete(fEnabledPackages);
   SafeDelete(fEnabledPackagesOnClient);
   SafeDelete(fLoadedMacros);
   SafeDelete(fPackageLock);
   SafeDelete(fGlobalPackageDirList);
   SafeDelete(fRecvMessages);
   SafeDelete(fInputData);
   SafeDelete(fRunningDSets);
   SafeDelete(fCloseMutex);

   // remove file with redirected logs
   if (TestBit(TProof::kIsClient)) {
      if (fLogFileR)
         fclose(fLogFileR);
      if (fLogFileW)
         fclose(fLogFileW);
      if (fLogFileName.Length())
         gSystem->Unlink(fLogFileName);
   }

   // Remove for the global list
   gROOT->GetListOfProofs()->Remove(this);
   if (gProof && gProof == this) {
      // Set previous as default
      TIter pvp(gROOT->GetListOfProofs(), kIterBackward);
      while ((gProof = (TProof *)pvp())) {
         if (gProof->InheritsFrom("TProof"))
            break;
      }
   }

   // For those interested in our destruction ...
   Emit("~TProof()");
}

//______________________________________________________________________________
Int_t TProof::Init(const char *, const char *conffile,
                   const char *confdir, Int_t loglevel, const char *alias)
{
   // Start the PROOF environment. Starting PROOF involves either connecting
   // to a master server, which in turn will start a set of slave servers, or
   // directly starting as master server (if master = ""). For a description
   // of the arguments see the TProof ctor. Returns the number of started
   // master or slave servers, returns 0 in case of error, in which case
   // fValid remains false.

   R__ASSERT(gSystem);

   fValid = kFALSE;

   // If in attach mode, options is filled with additiona info
   Bool_t attach = kFALSE;
   if (strlen(fUrl.GetOptions()) > 0) {
      attach = kTRUE;
      // A flag from the GUI
      TString opts = fUrl.GetOptions();
      if (opts.Contains("GUI")) {
         SetBit(TProof::kUsingSessionGui);
         opts.Remove(opts.Index("GUI"));
         fUrl.SetOptions(opts);
      }
   }

   if (TestBit(TProof::kIsMaster)) {
      // Fill default conf file and conf dir
      if (!conffile || strlen(conffile) == 0)
         fConfFile = kPROOF_ConfFile;
      if (!confdir  || strlen(confdir) == 0)
         fConfDir  = kPROOF_ConfDir;
   } else {
      fConfDir     = confdir;
      fConfFile    = conffile;
   }
   fWorkDir        = gSystem->WorkingDirectory();
   fLogLevel       = loglevel;
   fProtocol       = kPROOF_Protocol;
   fSendGroupView  = kTRUE;
   fImage          = fMasterServ ? "" : "<local>";
   fIntHandler     = 0;
   fStatus         = 0;
   fRecvMessages   = new TList;
   fRecvMessages->SetOwner(kTRUE);
   fSlaveInfo      = 0;
   fChains         = new TList;
   fAvailablePackages = 0;
   fEnabledPackages = 0;
   fRunningDSets   = 0;
   fEndMaster      = TestBit(TProof::kIsMaster) ? kTRUE : kFALSE;
   fInputData      = 0;
   ResetBit(TProof::kNewInputData);
   fPrintProgress  = 0;

   // Timeout for some collect actions
   fCollectTimeout = gEnv->GetValue("Proof.CollectTimeout", -1);

   // Should the workers be started dynamically; default: no
   fDynamicStartup = gEnv->GetValue("Proof.DynamicStartup", kFALSE);

   // Default entry point for the data pool is the master
   if (TestBit(TProof::kIsClient))
      fDataPoolUrl.Form("root://%s", fMaster.Data());
   else
      fDataPoolUrl = "";

   fProgressDialog        = 0;
   fProgressDialogStarted = kFALSE;

   // Default alias is the master name
   TString      al = (alias) ? alias : fMaster.Data();
   SetAlias(al);

   // Client logging of messages from the master and slaves
   fRedirLog = kFALSE;
   if (TestBit(TProof::kIsClient)) {
      fLogFileName    = "ProofLog_";
      if ((fLogFileW = gSystem->TempFileName(fLogFileName)) == 0)
         Error("Init", "could not create temporary logfile");
      if ((fLogFileR = fopen(fLogFileName, "r")) == 0)
         Error("Init", "could not open temp logfile for reading");
   }
   fLogToWindowOnly = kFALSE;

   // Status of cluster
   fNotIdle = 0;
   // Query type
   fSync = kTRUE;
   // Not enqueued
   fIsWaiting = kFALSE;

   // Counters
   fBytesRead = 0;
   fRealTime = 0;
   fCpuTime = 0;

   // List of queries
   fQueries = 0;
   fOtherQueries = 0;
   fDrawQueries = 0;
   fMaxDrawQueries = 1;
   fSeqNum = 0;

   // Remote ID of the session
   fSessionID = -1;

   // Part of active query
   fWaitingSlaves = 0;

   // Make remote PROOF player
   fPlayer = 0;
   MakePlayer();

   fFeedback = new TList;
   fFeedback->SetOwner();
   fFeedback->SetName("FeedbackList");
   AddInput(fFeedback);

   // sort slaves by descending performance index
   fSlaves           = new TSortedList(kSortDescending);
   fActiveSlaves     = new TList;
   fInactiveSlaves   = new TList;
   fUniqueSlaves     = new TList;
   fAllUniqueSlaves  = new TList;
   fNonUniqueMasters = new TList;
   fBadSlaves        = new TList;
   fAllMonitor       = new TMonitor;
   fActiveMonitor    = new TMonitor;
   fUniqueMonitor    = new TMonitor;
   fAllUniqueMonitor = new TMonitor;
   fCurrentMonitor   = 0;

   fPackageLock             = 0;
   fEnabledPackagesOnClient = 0;
   fLoadedMacros            = 0;
   fGlobalPackageDirList    = 0;

   if (IsMaster()) {
      // to make UploadPackage() method work on the master as well.
      fPackageDir = gProofServ->GetPackageDir();
   } else {

      TString sandbox = gEnv->GetValue("Proof.Sandbox", "");
      if (sandbox.IsNull()) {
         sandbox.Form("~/%s", kPROOF_WorkDir);
      }
      gSystem->ExpandPathName(sandbox);
      if (AssertPath(sandbox, kTRUE) != 0) {
         Error("Init", "failure asserting directory %s", sandbox.Data());
         return 0;
      }

      // Package Dir
      fPackageDir = gEnv->GetValue("Proof.PackageDir", "");
      if (fPackageDir.IsNull())
         fPackageDir.Form("%s/%s", sandbox.Data(), kPROOF_PackDir);
      if (AssertPath(fPackageDir, kTRUE) != 0) {
         Error("Init", "failure asserting directory %s", fPackageDir.Data());
         return 0;
      }
   }

   if (!IsMaster()) {
      // List of directories where to look for global packages
      TString globpack = gEnv->GetValue("Proof.GlobalPackageDirs","");
      if (globpack.Length() > 0) {
         Int_t ng = 0;
         Int_t from = 0;
         TString ldir;
         while (globpack.Tokenize(ldir, from, ":")) {
            if (gSystem->AccessPathName(ldir, kReadPermission)) {
               Warning("Init", "directory for global packages %s does not"
                               " exist or is not readable", ldir.Data());
            } else {
               // Add to the list, key will be "G<ng>", i.e. "G0", "G1", ...
               TString key = Form("G%d", ng++);
               if (!fGlobalPackageDirList) {
                  fGlobalPackageDirList = new THashList();
                  fGlobalPackageDirList->SetOwner();
               }
               fGlobalPackageDirList->Add(new TNamed(key,ldir));
            }
         }
      }

      TString lockpath(fPackageDir);
      lockpath.ReplaceAll("/", "%");
      lockpath.Insert(0, Form("%s/%s", gSystem->TempDirectory(), kPROOF_PackageLockFile));
      fPackageLock = new TProofLockPath(lockpath.Data());

      fEnabledPackagesOnClient = new TList;
      fEnabledPackagesOnClient->SetOwner();
   }

   // Master may want dynamic startup
   if (fDynamicStartup) {
      if (!IsMaster()) {
         // If on client - start the master
         if (!StartSlaves(attach))
            return 0;
      }
   } else {
      // Start slaves (the old, static, per-session way)
      if (!StartSlaves(attach))
         return 0;
   }

   if (fgSemaphore)
      SafeDelete(fgSemaphore);

   // we are now properly initialized
   fValid = kTRUE;

   // De-activate monitor (will be activated in Collect)
   fAllMonitor->DeActivateAll();

   // By default go into parallel mode
   GoParallel(9999, attach);

   // Send relevant initial state to slaves
   if (!attach)
      SendInitialState();
   else if (!IsIdle())
      // redirect log
      fRedirLog = kTRUE;

   // Done at this point, the alias will be communicated to the coordinator, if any
   if (TestBit(TProof::kIsClient))
      SetAlias(al);

   SetActive(kFALSE);

   if (IsValid()) {

      // Activate input handler
      ActivateAsyncInput();

      R__LOCKGUARD2(gROOTMutex);
      gROOT->GetListOfSockets()->Add(this);
   }
   return fActiveSlaves->GetSize();
}

//______________________________________________________________________________
Int_t TProof::AssertPath(const char *inpath, Bool_t writable)
{
   // Make sure that 'path' exists; if 'writable' is kTRUE, make also sure
   // that the path is writable

   if (!inpath || strlen(inpath) <= 0) {
      Error("AssertPath", "undefined input path");
      return -1;
   }

   TString path(inpath);
   gSystem->ExpandPathName(path);

   if (gSystem->AccessPathName(path, kFileExists)) {
      if (gSystem->mkdir(path, kTRUE) != 0) {
         Error("AssertPath", "could not create path %s", path.Data());
         return -1;
      }
   }
   // It must be writable
   if (gSystem->AccessPathName(path, kWritePermission) && writable) {
      if (gSystem->Chmod(path, 0666) != 0) {
         Error("AssertPath", "could not make path %s writable", path.Data());
         return -1;
      }
   }

   // Done
   return 0;
}

//______________________________________________________________________________
void TProof::SetManager(TProofMgr *mgr)
{
   // Set manager and schedule its destruction after this for clean
   // operations.

   fManager = mgr;

   if (mgr) {
      R__LOCKGUARD2(gROOTMutex);
      gROOT->GetListOfSockets()->Remove(mgr);
      gROOT->GetListOfSockets()->Add(mgr);
   }
}

//______________________________________________________________________________
Int_t TProof::AddWorkers(TList *workerList)
{
   // Works on the master node only.
   // It starts workers on the machines in workerList and sets the paths,
   // packages and macros as on the master.
   // It is a subbstitute for StartSlaves(...)
   // The code is mostly the master part of StartSlaves,
   // with the parallel startup removed.

   if (!IsMaster()) {
      Error("AddWorkers", "AddWorkers can only be called on the master!");
      return -1;
   }

   if (!workerList || !(workerList->GetSize())) {
      Error("AddWorkers", "The list of workers should not be empty; NULL: %d",
            workerList == 0);
      return -2;
   }

   // Code taken from master part of StartSlaves with the parllel part removed

   fImage = gProofServ->GetImage();
   if (fImage.IsNull())
      fImage = Form("%s:%s", TUrl(gSystem->HostName()).GetHostFQDN(),
                    gProofServ->GetWorkDir());

   // Get all workers
   UInt_t nSlaves = workerList->GetSize();
   UInt_t nSlavesDone = 0;
   Int_t ord = 0;

   // Loop over all workers and start them

   // a list of TSlave objects for workers that are being added
   TList *addedWorkers = new TList();
   addedWorkers->SetOwner(kFALSE);
   TListIter next(workerList);
   TObject *to;
   TProofNodeInfo *worker;
   while ((to = next())) {
      // Get the next worker from the list
      worker = (TProofNodeInfo *)to;

      // Read back worker node info
      const Char_t *image = worker->GetImage().Data();
      const Char_t *workdir = worker->GetWorkDir().Data();
      Int_t perfidx = worker->GetPerfIndex();
      Int_t sport = worker->GetPort();
      if (sport == -1)
         sport = fUrl.GetPort();

      // create slave server
      TString fullord;
      if (worker->GetOrdinal().Length() > 0) {
         fullord.Form("%s.%s", gProofServ->GetOrdinal(), worker->GetOrdinal().Data());
      } else {
         fullord.Form("%s.%d", gProofServ->GetOrdinal(), ord);
      }

      // create slave server
      TUrl u(Form("%s:%d",worker->GetNodeName().Data(), sport));
      // Add group info in the password firdl, if any
      if (strlen(gProofServ->GetGroup()) > 0) {
         // Set also the user, otherwise the password is not exported
         if (strlen(u.GetUser()) <= 0)
            u.SetUser(gProofServ->GetUser());
         u.SetPasswd(gProofServ->GetGroup());
      }
      TSlave *slave = CreateSlave(u.GetUrl(), fullord, perfidx,
                                  image, workdir);

      // Add to global list (we will add to the monitor list after
      // finalizing the server startup)
      Bool_t slaveOk = kTRUE;
      if (slave->IsValid()) {
         fSlaves->Add(slave);
         addedWorkers->Add(slave);
      } else {
         slaveOk = kFALSE;
         fBadSlaves->Add(slave);
      }

      PDB(kGlobal,3)
         Info("StartSlaves", "worker on host %s created"
              " and added to list", worker->GetNodeName().Data());

      // Notify opening of connection
      nSlavesDone++;
      TMessage m(kPROOF_SERVERSTARTED);
      m << TString("Opening connections to workers") << nSlaves
        << nSlavesDone << slaveOk;
      gProofServ->GetSocket()->Send(m);

      ord++;
   } //end of the worker loop

   // Cleanup
   SafeDelete(workerList);

   nSlavesDone = 0;

   // Here we finalize the server startup: in this way the bulk
   // of remote operations are almost parallelized
   TIter nxsl(addedWorkers);
   TSlave *sl = 0;
   while ((sl = (TSlave *) nxsl())) {

      // Finalize setup of the server
      if (sl->IsValid())
          sl->SetupServ(TSlave::kSlave, 0);

      // Monitor good slaves
      Bool_t slaveOk = kTRUE;
      if (sl->IsValid()) {
         fAllMonitor->Add(sl->GetSocket());
      } else {
         slaveOk = kFALSE;
         fBadSlaves->Add(sl);
      }

      // Notify end of startup operations
      nSlavesDone++;
      TMessage m(kPROOF_SERVERSTARTED);
      m << TString("Setting up worker servers") << nSlaves
        << nSlavesDone << slaveOk;
      gProofServ->GetSocket()->Send(m);
   }

   // Now set new state on the added workers (on all workers for simplicity)
   // use fEnabledPackages, fLoadedMacros,
   // gSystem->GetDynamicPath() and gSystem->GetIncludePath()
   // no need to load packages that are only loaded and not enabled (dyn mode)

   SetParallel(99999, 0);

   TList *tmpEnabledPackages = gProofServ->GetEnabledPackages();

   if (tmpEnabledPackages && tmpEnabledPackages->GetSize() > 0) {
      TIter nxp(tmpEnabledPackages);
      TObjString *os = 0;
      while ((os = (TObjString *) nxp())) {
         // Upload and Enable methods are intelligent and avoid
         // re-uploading or re-enabling of a package to a node that has it.
         UploadPackage(os->GetName());
         EnablePackage(os->GetName(), kTRUE);
      }
   }


   if (fLoadedMacros) {
      TIter nxp(fLoadedMacros);
      TObjString *os = 0;
      while ((os = (TObjString *) nxp())) {
         Printf("Loading a macro : %s", os->GetName());
         Load(os->GetName(), kTRUE, kTRUE, addedWorkers);
      }
   }

   TString dyn = gSystem->GetDynamicPath();
   dyn.ReplaceAll(":", " ");
   dyn.ReplaceAll("\"", " ");
   AddDynamicPath(dyn, addedWorkers);
   TString inc = gSystem->GetIncludePath();
   inc.ReplaceAll("-I", " ");
   inc.ReplaceAll("\"", " ");
   AddIncludePath(inc, addedWorkers);

   // Cleanup
   delete addedWorkers;

   // Inform the client that the number of workers is changed
   if (fDynamicStartup && gProofServ)
      gProofServ->SendParallel(kTRUE);

   return 0;
}

//______________________________________________________________________________
Int_t TProof::RemoveWorkers(TList *workerList)
{
   // Used for shuting down the workres after a query is finished.
   // Sends each of the workers from the workerList, a kPROOF_STOP message.
   // If the workerList == 0, shutdown all the workers.

   if (!IsMaster()) {
      Error("RemoveWorkers", "RemoveWorkers can only be called on the master!");
      return -1;
   }

   fFileMap.clear(); // This could be avoided if CopyFromCache was used in SendFile

   if (!workerList) {
      // shutdown all the workers
      TIter nxsl(fSlaves);
      TSlave *sl = 0;
      while ((sl = (TSlave *) nxsl())) {
         // Shut down the worker assumig that it is not processing
         TerminateWorker(sl);
      }

   } else {
      if (!(workerList->GetSize())) {
         Error("RemoveWorkers", "The list of workers should not be empty!");
         return -2;
      }

      // Loop over all the workers and stop them
      TListIter next(workerList);
      TObject *to;
      TProofNodeInfo *worker;
      while ((to = next())) {
         TSlave *sl = 0;
         if (!strcmp(to->ClassName(), "TProofNodeInfo")) {
            // Get the next worker from the list
            worker = (TProofNodeInfo *)to;
            TIter nxsl(fSlaves);
            while ((sl = (TSlave *) nxsl())) {
               // Shut down the worker assumig that it is not processing
               if (sl->GetName() == worker->GetNodeName())
                  break;
            }
         } else if (to->InheritsFrom("TSlave")) {
            sl = (TSlave *) to;
         } else {
            Warning("RemoveWorkers","unknown object type: %s - it should be"
                  " TProofNodeInfo or inheriting from TSlave", to->ClassName());
         }
         // Shut down the worker assumig that it is not processing
         if (sl) {
            if (gDebug > 0)
               Info("RemoveWorkers","terminating worker %s", sl->GetOrdinal());
            TerminateWorker(sl);
         }
      }
   }

   // Update also the master counter
   if (gProofServ && fSlaves->GetSize() <= 0) gProofServ->ReleaseWorker("master");

   return 0;
}

//______________________________________________________________________________
Bool_t TProof::StartSlaves(Bool_t attach)
{
   // Start up PROOF slaves.

   // If this is a master server, find the config file and start slave
   // servers as specified in the config file
   if (TestBit(TProof::kIsMaster)) {

      Int_t pc = 0;
      TList *workerList = new TList;
      // Get list of workers
      if (gProofServ->GetWorkers(workerList, pc) == TProofServ::kQueryStop) {
         TString emsg("no resource currently available for this session: please retry later");
         if (gDebug > 0) Info("StartSlaves", emsg.Data());
         gProofServ->SendAsynMessage(emsg.Data());
         return kFALSE;
      }

      // Setup the workers
      if (AddWorkers(workerList) < 0)
         return kFALSE;

   } else {

      // create master server
      Printf("Starting master: opening connection ... ");
      TSlave *slave = CreateSubmaster(fUrl.GetUrl(), "0", "master", 0);

      if (slave->IsValid()) {

         // Notify
         fprintf(stderr,"Starting master:"
                        " connection open: setting up server ...             \r");
         StartupMessage("Connection to master opened", kTRUE, 1, 1);

         if (!attach) {

            // Set worker interrupt handler
            slave->SetInterruptHandler(kTRUE);

            // Finalize setup of the server
            slave->SetupServ(TSlave::kMaster, fConfFile);

            if (slave->IsValid()) {

               // Notify
               Printf("Starting master: OK                                     ");
               StartupMessage("Master started", kTRUE, 1, 1);

               // check protocol compatibility
               // protocol 1 is not supported anymore
               if (fProtocol == 1) {
                  Error("StartSlaves",
                        "client and remote protocols not compatible (%d and %d)",
                        kPROOF_Protocol, fProtocol);
                  slave->Close("S");
                  delete slave;
                  return kFALSE;
               }

               fSlaves->Add(slave);
               fAllMonitor->Add(slave->GetSocket());

               // Unset worker interrupt handler
               slave->SetInterruptHandler(kFALSE);

               // Set interrupt PROOF handler from now on
               fIntHandler = new TProofInterruptHandler(this);

               // Give-up after 5 minutes
               Int_t rc = Collect(slave, 300);
               Int_t slStatus = slave->GetStatus();
               if (slStatus == -99 || slStatus == -98 || rc == 0) {
                  fSlaves->Remove(slave);
                  fAllMonitor->Remove(slave->GetSocket());
                  if (slStatus == -99)
                     Error("StartSlaves", "no resources available or problems setting up workers (check logs)");
                  else if (slStatus == -98)
                     Error("StartSlaves", "could not setup output redirection on master");
                  else
                     Error("StartSlaves", "setting up master");
                  slave->Close("S");
                  delete slave;
                  return 0;
               }

               if (!slave->IsValid()) {
                  fSlaves->Remove(slave);
                  fAllMonitor->Remove(slave->GetSocket());
                  slave->Close("S");
                  delete slave;
                  Error("StartSlaves",
                        "failed to setup connection with PROOF master server");
                  return kFALSE;
               }

               if (!gROOT->IsBatch()) {
                  if ((fProgressDialog =
                     gROOT->GetPluginManager()->FindHandler("TProofProgressDialog")))
                     if (fProgressDialog->LoadPlugin() == -1)
                        fProgressDialog = 0;
               }
            } else {
               // Notify
               Printf("Starting master: failure");
            }
         } else {

            // Notify
            Printf("Starting master: OK                                     ");
            StartupMessage("Master attached", kTRUE, 1, 1);

            if (!gROOT->IsBatch()) {
               if ((fProgressDialog =
                  gROOT->GetPluginManager()->FindHandler("TProofProgressDialog")))
                  if (fProgressDialog->LoadPlugin() == -1)
                     fProgressDialog = 0;
            }

            fSlaves->Add(slave);
            fAllMonitor->Add(slave->GetSocket());

            fIntHandler = new TProofInterruptHandler(this);
         }

      } else {
         delete slave;
         // Notify only if verbosity is on: most likely the failure has already been notified
         if (gDebug > 0)
            Error("StartSlaves", "failed to create (or connect to) the PROOF master server");
         return kFALSE;
      }
   }

   return kTRUE;
}

//______________________________________________________________________________
void TProof::Close(Option_t *opt)
{
   // Close all open slave servers.
   // Client can decide to shutdown the remote session by passing option is 'S'
   // or 's'. Default for clients is detach, if supported. Masters always
   // shutdown the remote counterpart.

   {  R__LOCKGUARD2(fCloseMutex);

      fValid = kFALSE;
      if (fSlaves) {
         if (fIntHandler)
            fIntHandler->Remove();

         TIter nxs(fSlaves);
         TSlave *sl = 0;
         while ((sl = (TSlave *)nxs()))
            sl->Close(opt);

         fActiveSlaves->Clear("nodelete");
         fUniqueSlaves->Clear("nodelete");
         fAllUniqueSlaves->Clear("nodelete");
         fNonUniqueMasters->Clear("nodelete");
         fBadSlaves->Clear("nodelete");
         fSlaves->Delete();
      }
   }

   {
      R__LOCKGUARD2(gROOTMutex);
      gROOT->GetListOfSockets()->Remove(this);

      if (IsProofd()) {

         gROOT->GetListOfProofs()->Remove(this);
         if (gProof && gProof == this) {
            // Set previous proofd-related as default
            TIter pvp(gROOT->GetListOfProofs(), kIterBackward);
            while ((gProof = (TProof *)pvp())) {
               if (gProof->IsProofd())
                  break;
            }
         }
      }
   }
}

//______________________________________________________________________________
TSlave *TProof::CreateSlave(const char *url, const char *ord,
                            Int_t perf, const char *image, const char *workdir)
{
   // Create a new TSlave of type TSlave::kSlave.
   // Note: creation of TSlave is private with TProof as a friend.
   // Derived classes must use this function to create slaves.

   TSlave* sl = TSlave::Create(url, ord, perf, image,
                               this, TSlave::kSlave, workdir, 0);

   if (sl->IsValid()) {
      sl->SetInputHandler(new TProofInputHandler(this, sl->GetSocket()));
      // must set fParallel to 1 for slaves since they do not
      // report their fParallel with a LOG_DONE message
      sl->fParallel = 1;
   }

   return sl;
}

//______________________________________________________________________________
TSlave *TProof::CreateSubmaster(const char *url, const char *ord,
                                const char *image, const char *msd)
{
   // Create a new TSlave of type TSlave::kMaster.
   // Note: creation of TSlave is private with TProof as a friend.
   // Derived classes must use this function to create slaves.

   TSlave *sl = TSlave::Create(url, ord, 100, image, this,
                               TSlave::kMaster, 0, msd);

   if (sl->IsValid()) {
      sl->SetInputHandler(new TProofInputHandler(this, sl->GetSocket()));
   }

   return sl;
}

//______________________________________________________________________________
TSlave *TProof::FindSlave(TSocket *s) const
{
   // Find slave that has TSocket s. Returns 0 in case slave is not found.

   TSlave *sl;
   TIter   next(fSlaves);

   while ((sl = (TSlave *)next())) {
      if (sl->IsValid() && sl->GetSocket() == s)
         return sl;
   }
   return 0;
}

//______________________________________________________________________________
void TProof::FindUniqueSlaves()
{
   // Add to the fUniqueSlave list the active slaves that have a unique
   // (user) file system image. This information is used to transfer files
   // only once to nodes that share a file system (an image). Submasters
   // which are not in fUniqueSlaves are put in the fNonUniqueMasters
   // list. That list is used to trigger the transferring of files to
   // the submaster's unique slaves without the need to transfer the file
   // to the submaster.

   fUniqueSlaves->Clear();
   fUniqueMonitor->RemoveAll();
   fAllUniqueSlaves->Clear();
   fAllUniqueMonitor->RemoveAll();
   fNonUniqueMasters->Clear();

   TIter next(fActiveSlaves);

   while (TSlave *sl = dynamic_cast<TSlave*>(next())) {
      if (fImage == sl->fImage) {
         if (sl->GetSlaveType() == TSlave::kMaster) {
            fNonUniqueMasters->Add(sl);
            fAllUniqueSlaves->Add(sl);
            fAllUniqueMonitor->Add(sl->GetSocket());
         }
         continue;
      }

      TIter next2(fUniqueSlaves);
      TSlave *replace_slave = 0;
      Bool_t add = kTRUE;
      while (TSlave *sl2 = dynamic_cast<TSlave*>(next2())) {
         if (sl->fImage == sl2->fImage) {
            add = kFALSE;
            if (sl->GetSlaveType() == TSlave::kMaster) {
               if (sl2->GetSlaveType() == TSlave::kSlave) {
                  // give preference to master
                  replace_slave = sl2;
                  add = kTRUE;
               } else if (sl2->GetSlaveType() == TSlave::kMaster) {
                  fNonUniqueMasters->Add(sl);
                  fAllUniqueSlaves->Add(sl);
                  fAllUniqueMonitor->Add(sl->GetSocket());
               } else {
                  Error("FindUniqueSlaves", "TSlave is neither Master nor Slave");
                  R__ASSERT(0);
               }
            }
            break;
         }
      }

      if (add) {
         fUniqueSlaves->Add(sl);
         fAllUniqueSlaves->Add(sl);
         fUniqueMonitor->Add(sl->GetSocket());
         fAllUniqueMonitor->Add(sl->GetSocket());
         if (replace_slave) {
            fUniqueSlaves->Remove(replace_slave);
            fAllUniqueSlaves->Remove(replace_slave);
            fUniqueMonitor->Remove(replace_slave->GetSocket());
            fAllUniqueMonitor->Remove(replace_slave->GetSocket());
         }
      }
   }

   // will be actiavted in Collect()
   fUniqueMonitor->DeActivateAll();
   fAllUniqueMonitor->DeActivateAll();
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfSlaves() const
{
   // Return number of slaves as described in the config file.

   return fSlaves->GetSize();
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfActiveSlaves() const
{
   // Return number of active slaves, i.e. slaves that are valid and in
   // the current computing group.

   return fActiveSlaves->GetSize();
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfInactiveSlaves() const
{
   // Return number of inactive slaves, i.e. slaves that are valid but not in
   // the current computing group.

   return fInactiveSlaves->GetSize();
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfUniqueSlaves() const
{
   // Return number of unique slaves, i.e. active slaves that have each a
   // unique different user files system.

   return fUniqueSlaves->GetSize();
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfBadSlaves() const
{
   // Return number of bad slaves. This are slaves that we in the config
   // file, but refused to startup or that died during the PROOF session.

   return fBadSlaves->GetSize();
}

//______________________________________________________________________________
void TProof::AskStatistics()
{
   // Ask the for the statistics of the slaves.

   if (!IsValid()) return;

   Broadcast(kPROOF_GETSTATS, kActive);
   Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
void TProof::AskParallel()
{
   // Ask the for the number of parallel slaves.

   if (!IsValid()) return;

   Broadcast(kPROOF_GETPARALLEL, kActive);
   Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
TList *TProof::GetListOfQueries(Option_t *opt)
{
   // Ask the master for the list of queries.

   if (!IsValid() || TestBit(TProof::kIsMaster)) return (TList *)0;

   Bool_t all = ((strchr(opt,'A') || strchr(opt,'a'))) ? kTRUE : kFALSE;
   TMessage m(kPROOF_QUERYLIST);
   m << all;
   Broadcast(m, kActive);
   Collect(kActive, fCollectTimeout);

   // This should have been filled by now
   return fQueries;
}

//______________________________________________________________________________
Int_t TProof::GetNumberOfQueries()
{
   // Number of queries processed by this session

   if (fQueries)
      return fQueries->GetSize() - fOtherQueries;
   return 0;
}

//______________________________________________________________________________
void TProof::SetMaxDrawQueries(Int_t max)
{
   // Set max number of draw queries whose results are saved

   if (max > 0) {
      if (fPlayer)
         fPlayer->SetMaxDrawQueries(max);
      fMaxDrawQueries = max;
   }
}

//______________________________________________________________________________
void TProof::GetMaxQueries()
{
   // Get max number of queries whose full results are kept in the
   // remote sandbox

   TMessage m(kPROOF_MAXQUERIES);
   m << kFALSE;
   Broadcast(m, kActive);
   Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
TList *TProof::GetQueryResults()
{
   // Return pointer to the list of query results in the player

   return (fPlayer ? fPlayer->GetListOfResults() : (TList *)0);
}

//______________________________________________________________________________
TQueryResult *TProof::GetQueryResult(const char *ref)
{
   // Return pointer to the full TQueryResult instance owned by the player
   // and referenced by 'ref'. If ref = 0 or "", return the last query result.

   return (fPlayer ? fPlayer->GetQueryResult(ref) : (TQueryResult *)0);
}

//______________________________________________________________________________
void TProof::ShowQueries(Option_t *opt)
{
   // Ask the master for the list of queries.
   // Options:
   //           "A"     show information about all the queries known to the
   //                   server, i.e. even those processed by other sessions
   //           "L"     show only information about queries locally available
   //                   i.e. already retrieved. If "L" is specified, "A" is
   //                   ignored.
   //           "F"     show all details available about queries
   //           "H"     print help menu
   // Default ""

   Bool_t help = ((strchr(opt,'H') || strchr(opt,'h'))) ? kTRUE : kFALSE;
   if (help) {

      // Help

      Printf("+++");
      Printf("+++ Options: \"A\" show all queries known to server");
      Printf("+++          \"L\" show retrieved queries");
      Printf("+++          \"F\" full listing of query info");
      Printf("+++          \"H\" print this menu");
      Printf("+++");
      Printf("+++ (case insensitive)");
      Printf("+++");
      Printf("+++ Use Retrieve(<#>) to retrieve the full"
             " query results from the master");
      Printf("+++     e.g. Retrieve(8)");

      Printf("+++");

      return;
   }

   if (!IsValid()) return;

   Bool_t local = ((strchr(opt,'L') || strchr(opt,'l'))) ? kTRUE : kFALSE;

   TObject *pq = 0;
   if (!local) {
      GetListOfQueries(opt);

      if (!fQueries) return;

      TIter nxq(fQueries);

      // Queries processed by other sessions
      if (fOtherQueries > 0) {
         Printf("+++");
         Printf("+++ Queries processed during other sessions: %d", fOtherQueries);
         Int_t nq = 0;
         while (nq++ < fOtherQueries && (pq = nxq()))
            pq->Print(opt);
      }

      // Queries processed by this session
      Printf("+++");
      Printf("+++ Queries processed during this session: selector: %d, draw: %d",
              GetNumberOfQueries(), fDrawQueries);
      while ((pq = nxq()))
         pq->Print(opt);

   } else {

      // Queries processed by this session
      Printf("+++");
      Printf("+++ Queries processed during this session: selector: %d, draw: %d",
              GetNumberOfQueries(), fDrawQueries);

      // Queries available locally
      TList *listlocal = fPlayer ? fPlayer->GetListOfResults() : (TList *)0;
      if (listlocal) {
         Printf("+++");
         Printf("+++ Queries available locally: %d", listlocal->GetSize());
         TIter nxlq(listlocal);
         while ((pq = nxlq()))
            pq->Print(opt);
      }
   }
   Printf("+++");
}

//______________________________________________________________________________
Bool_t TProof::IsDataReady(Long64_t &totalbytes, Long64_t &bytesready)
{
   // See if the data is ready to be analyzed.

   if (!IsValid()) return kFALSE;

   TList submasters;
   TIter nextSlave(GetListOfActiveSlaves());
   while (TSlave *sl = dynamic_cast<TSlave*>(nextSlave())) {
      if (sl->GetSlaveType() == TSlave::kMaster) {
         submasters.Add(sl);
      }
   }

   fDataReady = kTRUE; //see if any submasters set it to false
   fBytesReady = 0;
   fTotalBytes = 0;
   //loop over submasters and see if data is ready
   if (submasters.GetSize() > 0) {
      Broadcast(kPROOF_DATA_READY, &submasters);
      Collect(&submasters);
   }

   bytesready = fBytesReady;
   totalbytes = fTotalBytes;

   EmitVA("IsDataReady(Long64_t,Long64_t)", 2, totalbytes, bytesready);

   //PDB(kGlobal,2)
   Info("IsDataReady", "%lld / %lld (%s)",
        bytesready, totalbytes, fDataReady?"READY":"NOT READY");

   return fDataReady;
}

//______________________________________________________________________________
void TProof::Interrupt(EUrgent type, ESlaves list)
{
   // Send interrupt to master or slave servers.

   if (!IsValid()) return;

   TList *slaves = 0;
   if (list == kAll)       slaves = fSlaves;
   if (list == kActive)    slaves = fActiveSlaves;
   if (list == kUnique)    slaves = fUniqueSlaves;
   if (list == kAllUnique) slaves = fAllUniqueSlaves;

   if (slaves->GetSize() == 0) return;

   TSlave *sl;
   TIter   next(slaves);

   while ((sl = (TSlave *)next())) {
      if (sl->IsValid()) {

         // Ask slave to progate the interrupt request
         sl->Interrupt((Int_t)type);
      }
   }
}

//______________________________________________________________________________
Int_t TProof::GetParallel() const
{
   // Returns number of slaves active in parallel mode. Returns 0 in case
   // there are no active slaves. Returns -1 in case of error.

   if (!IsValid()) return -1;

   // iterate over active slaves and return total number of slaves
   TIter nextSlave(GetListOfActiveSlaves());
   Int_t nparallel = 0;
   while (TSlave* sl = dynamic_cast<TSlave*>(nextSlave()))
      if (sl->GetParallel() >= 0)
         nparallel += sl->GetParallel();

   return nparallel;
}

//______________________________________________________________________________
TList *TProof::GetListOfSlaveInfos()
{
   // Returns list of TSlaveInfo's. In case of error return 0.

   if (!IsValid()) return 0;

   if (fSlaveInfo == 0) {
      fSlaveInfo = new TSortedList(kSortDescending);
      fSlaveInfo->SetOwner();
   } else {
      fSlaveInfo->Delete();
   }

   TList masters;
   TIter next(GetListOfSlaves());
   TSlave *slave;

   while ((slave = (TSlave *) next()) != 0) {
      if (slave->GetSlaveType() == TSlave::kSlave) {
         TSlaveInfo *slaveinfo = new TSlaveInfo(slave->GetOrdinal(),
                                                slave->GetName(),
                                                slave->GetPerfIdx());
         fSlaveInfo->Add(slaveinfo);

         TIter nextactive(GetListOfActiveSlaves());
         TSlave *activeslave;
         while ((activeslave = (TSlave *) nextactive())) {
            if (TString(slaveinfo->GetOrdinal()) == activeslave->GetOrdinal()) {
               slaveinfo->SetStatus(TSlaveInfo::kActive);
               break;
            }
         }

         TIter nextbad(GetListOfBadSlaves());
         TSlave *badslave;
         while ((badslave = (TSlave *) nextbad())) {
            if (TString(slaveinfo->GetOrdinal()) == badslave->GetOrdinal()) {
               slaveinfo->SetStatus(TSlaveInfo::kBad);
               break;
            }
         }

      } else if (slave->GetSlaveType() == TSlave::kMaster) {
         if (slave->IsValid()) {
            if (slave->GetSocket()->Send(kPROOF_GETSLAVEINFO) == -1)
               MarkBad(slave, "could not send kPROOF_GETSLAVEINFO message");
            else
               masters.Add(slave);
         }
      } else {
         Error("GetSlaveInfo", "TSlave is neither Master nor Slave");
         R__ASSERT(0);
      }
   }
   if (masters.GetSize() > 0) Collect(&masters);

   return fSlaveInfo;
}

//______________________________________________________________________________
void TProof::Activate(TList *slaves)
{
   // Activate slave server list.

   TMonitor *mon = fAllMonitor;
   mon->DeActivateAll();

   slaves = !slaves ? fActiveSlaves : slaves;

   TIter next(slaves);
   TSlave *sl;
   while ((sl = (TSlave*) next())) {
      if (sl->IsValid())
         mon->Activate(sl->GetSocket());
   }
}

//______________________________________________________________________________
void TProof::SetMonitor(TMonitor *mon, Bool_t on)
{
   // Activate (on == TRUE) or deactivate (on == FALSE) all sockets
   // monitored by 'mon'.

   TMonitor *m = (mon) ? mon : fCurrentMonitor;
   if (m) {
      if (on)
         m->ActivateAll();
      else
         m->DeActivateAll();
   }
}

//______________________________________________________________________________
Int_t TProof::BroadcastGroupPriority(const char *grp, Int_t priority, TList *workers)
{
   // Broadcast the group priority to all workers in the specified list. Returns
   // the number of workers the message was successfully sent to.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (workers->GetSize() == 0) return 0;

   int   nsent = 0;
   TIter next(workers);

   TSlave *wrk;
   while ((wrk = (TSlave *)next())) {
      if (wrk->IsValid()) {
         if (wrk->SendGroupPriority(grp, priority) == -1)
            MarkBad(wrk, "could not send group priority");
         else
            nsent++;
      }
   }

   return nsent;
}

//______________________________________________________________________________
Int_t TProof::BroadcastGroupPriority(const char *grp, Int_t priority, ESlaves list)
{
   // Broadcast the group priority to all workers in the specified list. Returns
   // the number of workers the message was successfully sent to.
   // Returns -1 in case of error.

   TList *workers = 0;
   if (list == kAll)       workers = fSlaves;
   if (list == kActive)    workers = fActiveSlaves;
   if (list == kUnique)    workers = fUniqueSlaves;
   if (list == kAllUnique) workers = fAllUniqueSlaves;

   return BroadcastGroupPriority(grp, priority, workers);
}

//______________________________________________________________________________
Int_t TProof::Broadcast(const TMessage &mess, TList *slaves)
{
   // Broadcast a message to all slaves in the specified list. Returns
   // the number of slaves the message was successfully sent to.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (!slaves || slaves->GetSize() == 0) return 0;

   int   nsent = 0;
   TIter next(slaves);

   TSlave *sl;
   while ((sl = (TSlave *)next())) {
      if (sl->IsValid()) {
         if (sl->GetSocket()->Send(mess) == -1)
            MarkBad(sl, "could not broadcast request");
         else
            nsent++;
      }
   }

   return nsent;
}

//______________________________________________________________________________
Int_t TProof::Broadcast(const TMessage &mess, ESlaves list)
{
   // Broadcast a message to all slaves in the specified list (either
   // all slaves or only the active slaves). Returns the number of slaves
   // the message was successfully sent to. Returns -1 in case of error.

   TList *slaves = 0;
   if (list == kAll)       slaves = fSlaves;
   if (list == kActive)    slaves = fActiveSlaves;
   if (list == kUnique)    slaves = fUniqueSlaves;
   if (list == kAllUnique) slaves = fAllUniqueSlaves;

   return Broadcast(mess, slaves);
}

//______________________________________________________________________________
Int_t TProof::Broadcast(const char *str, Int_t kind, TList *slaves)
{
   // Broadcast a character string buffer to all slaves in the specified
   // list. Use kind to set the TMessage what field. Returns the number of
   // slaves the message was sent to. Returns -1 in case of error.

   TMessage mess(kind);
   if (str) mess.WriteString(str);
   return Broadcast(mess, slaves);
}

//______________________________________________________________________________
Int_t TProof::Broadcast(const char *str, Int_t kind, ESlaves list)
{
   // Broadcast a character string buffer to all slaves in the specified
   // list (either all slaves or only the active slaves). Use kind to
   // set the TMessage what field. Returns the number of slaves the message
   // was sent to. Returns -1 in case of error.

   TMessage mess(kind);
   if (str) mess.WriteString(str);
   return Broadcast(mess, list);
}

//______________________________________________________________________________
Int_t TProof::BroadcastObject(const TObject *obj, Int_t kind, TList *slaves)
{
   // Broadcast an object to all slaves in the specified list. Use kind to
   // set the TMEssage what field. Returns the number of slaves the message
   // was sent to. Returns -1 in case of error.

   TMessage mess(kind);
   mess.WriteObject(obj);
   return Broadcast(mess, slaves);
}

//______________________________________________________________________________
Int_t TProof::BroadcastObject(const TObject *obj, Int_t kind, ESlaves list)
{
   // Broadcast an object to all slaves in the specified list. Use kind to
   // set the TMEssage what field. Returns the number of slaves the message
   // was sent to. Returns -1 in case of error.

   TMessage mess(kind);
   mess.WriteObject(obj);
   return Broadcast(mess, list);
}

//______________________________________________________________________________
Int_t TProof::BroadcastRaw(const void *buffer, Int_t length, TList *slaves)
{
   // Broadcast a raw buffer of specified length to all slaves in the
   // specified list. Returns the number of slaves the buffer was sent to.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (slaves->GetSize() == 0) return 0;

   int   nsent = 0;
   TIter next(slaves);

   TSlave *sl;
   while ((sl = (TSlave *)next())) {
      if (sl->IsValid()) {
         if (sl->GetSocket()->SendRaw(buffer, length) == -1)
            MarkBad(sl, "could not send broadcast-raw request");
         else
            nsent++;
      }
   }

   return nsent;
}

//______________________________________________________________________________
Int_t TProof::BroadcastRaw(const void *buffer, Int_t length, ESlaves list)
{
   // Broadcast a raw buffer of specified length to all slaves in the
   // specified list. Returns the number of slaves the buffer was sent to.
   // Returns -1 in case of error.

   TList *slaves = 0;
   if (list == kAll)       slaves = fSlaves;
   if (list == kActive)    slaves = fActiveSlaves;
   if (list == kUnique)    slaves = fUniqueSlaves;
   if (list == kAllUnique) slaves = fAllUniqueSlaves;

   return BroadcastRaw(buffer, length, slaves);
}

//______________________________________________________________________________
Int_t TProof::BroadcastFile(const char *file, Int_t opt, const char *rfile, TList *wrks)
{
   // Broadcast file to all workers in the specified list. Returns the number of workers
   // the buffer was sent to.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (wrks->GetSize() == 0) return 0;

   int   nsent = 0;
   TIter next(wrks);

   TSlave *wrk;
   while ((wrk = (TSlave *)next())) {
      if (wrk->IsValid()) {
         if (SendFile(file, opt, rfile, wrk) < 0)
            Error("BroadcastFile",
                  "problems sending file to worker %s (%s)",
                  wrk->GetOrdinal(), wrk->GetName());
         else
            nsent++;
      }
   }

   return nsent;
}

//______________________________________________________________________________
Int_t TProof::BroadcastFile(const char *file, Int_t opt, const char *rfile, ESlaves list)
{
   // Broadcast file to all workers in the specified list. Returns the number of workers
   // the buffer was sent to.
   // Returns -1 in case of error.

   TList *wrks = 0;
   if (list == kAll)       wrks = fSlaves;
   if (list == kActive)    wrks = fActiveSlaves;
   if (list == kUnique)    wrks = fUniqueSlaves;
   if (list == kAllUnique) wrks = fAllUniqueSlaves;

   return BroadcastFile(file, opt, rfile, wrks);
}

//______________________________________________________________________________
void TProof::ReleaseMonitor(TMonitor *mon)
{
   // Release the used monitor to be used, making sure to delete newly created
   // monitors.

   if (mon && (mon != fAllMonitor) && (mon != fActiveMonitor)
           && (mon != fUniqueMonitor) && (mon != fAllUniqueMonitor)) {
      delete mon;
   }
}

//______________________________________________________________________________
Int_t TProof::Collect(const TSlave *sl, Long_t timeout, Int_t endtype)
{
   // Collect responses from slave sl. Returns the number of slaves that
   // responded (=1).
   // If timeout >= 0, wait at most timeout seconds (timeout = -1 by default,
   // which means wait forever).
   // If defined (>= 0) endtype is the message that stops this collection.

   Int_t rc = 0;

   TMonitor *mon = 0;
   if (!sl->IsValid()) return 0;

   if (fCurrentMonitor == fAllMonitor) {
      mon = new TMonitor;
   } else {
      mon = fAllMonitor;
      mon->DeActivateAll();
   }
   mon->Activate(sl->GetSocket());

   rc = Collect(mon, timeout, endtype);
   ReleaseMonitor(mon);
   return rc;
}

//______________________________________________________________________________
Int_t TProof::Collect(TList *slaves, Long_t timeout, Int_t endtype)
{
   // Collect responses from the slave servers. Returns the number of slaves
   // that responded.
   // If timeout >= 0, wait at most timeout seconds (timeout = -1 by default,
   // which means wait forever).
   // If defined (>= 0) endtype is the message that stops this collection.

   Int_t rc = 0;

   TMonitor *mon = 0;

   if (fCurrentMonitor == fAllMonitor) {
      mon = new TMonitor;
   } else {
      mon = fAllMonitor;
      mon->DeActivateAll();
   }
   TIter next(slaves);
   TSlave *sl;
   while ((sl = (TSlave*) next())) {
      if (sl->IsValid())
         mon->Activate(sl->GetSocket());
   }

   rc = Collect(mon, timeout, endtype);
   ReleaseMonitor(mon);
   return rc;
}

//______________________________________________________________________________
Int_t TProof::Collect(ESlaves list, Long_t timeout, Int_t endtype)
{
   // Collect responses from the slave servers. Returns the number of slaves
   // that responded.
   // If timeout >= 0, wait at most timeout seconds (timeout = -1 by default,
   // which means wait forever).
   // If defined (>= 0) endtype is the message that stops this collection.

   Int_t rc = 0;
   TMonitor *mon = 0;

   if (list == kAll)       mon = fAllMonitor;
   if (list == kActive)    mon = fActiveMonitor;
   if (list == kUnique)    mon = fUniqueMonitor;
   if (list == kAllUnique) mon = fAllUniqueMonitor;
   if (fCurrentMonitor == mon) {
      // Get a copy
      mon = new TMonitor(*mon);
   }
   mon->ActivateAll();

   rc = Collect(mon, timeout, endtype);
   ReleaseMonitor(mon);
   return rc;
}

//______________________________________________________________________________
Int_t TProof::Collect(TMonitor *mon, Long_t timeout, Int_t endtype)
{
   // Collect responses from the slave servers. Returns the number of messages
   // received. Can be 0 if there are no active slaves.
   // If timeout >= 0, wait at most timeout seconds (timeout = -1 by default,
   // which means wait forever).
   // If defined (>= 0) endtype is the message that stops this collection.

   // Reset the status flag and clear the messages in the list, if any
   fStatus = 0;
   fRecvMessages->Clear();

   Long_t actto = (Long_t)(gEnv->GetValue("Proof.SocketActivityTimeout", 600) * 1000);

   if (!mon->GetActive(actto)) return 0;

   DeActivateAsyncInput();

   // Used by external code to know what we are monitoring
   TMonitor *savedMonitor = 0;
   if (fCurrentMonitor) {
      savedMonitor = fCurrentMonitor;
      fCurrentMonitor = mon;
   } else {
      fCurrentMonitor = mon;
      fBytesRead = 0;
      fRealTime  = 0.0;
      fCpuTime   = 0.0;
   }

   // We want messages on the main window during synchronous collection,
   // but we save the present status to restore it at the end
   Bool_t saveRedirLog = fRedirLog;
   if (!IsIdle() && !IsSync())
      fRedirLog = kFALSE;

   int cnt = 0, rc = 0;

   // Timeout counter
   Long_t nto = timeout;
   if (gDebug > 2)
      Info("Collect","active: %d", mon->GetActive());

   // On clients, handle Ctrl-C during collection
   if (fIntHandler)
      fIntHandler->Add();

   // Sockets w/o activity during the last 'sto' millisecs are deactivated
   Long_t sto = -1;
   Int_t nsto = 60;
   while (mon->GetActive(sto) && (nto < 0 || nto > 0)) {

      // Wait for a ready socket
      TSocket *s = mon->Select(1000);

      if (s && s != (TSocket *)(-1)) {
         // Get and analyse the info it did receive
         rc = CollectInputFrom(s, endtype);
         if (rc  == 1 || (rc == 2 && !savedMonitor)) {
            // Deactivate it if we are done with it
            mon->DeActivate(s);
            PDB(kGlobal, 2)
               Info("Collect","deactivating %p (active: %d, %p)",
                              s, mon->GetActive(),
                              mon->GetListOfActives()->First());
         } else if (rc == 2) {
            // This end message was for the saved monitor
            // Deactivate it if we are done with it
            if (savedMonitor) {
               savedMonitor->DeActivate(s);
               PDB(kGlobal, 2)
                  Info("Collect","save monitor: deactivating %p (active: %d, %p)",
                                 s, savedMonitor->GetActive(),
                                 savedMonitor->GetListOfActives()->First());
            }
         }

         // Update counter (if no error occured)
         if (rc >= 0)
            cnt++;
      } else {
         // If not timed-out, exit if not stopped or not aborted
         // (player exits status is finished in such a case); otherwise,
         // we still need to collect the partial output info
         if (!s)
            if (fPlayer && (fPlayer->GetExitStatus() == TVirtualProofPlayer::kFinished))
               mon->DeActivateAll();
         // Decrease the timeout counter if requested
         if (s == (TSocket *)(-1) && nto > 0)
            nto--;
      }
      // Check if we need to check the socket activity (we do it every 10 cycles ~ 10 sec)
      sto = -1;
      if (--nsto <= 0) {
         sto = (Long_t) actto;
         nsto = 60;
      }
   }

   // If timed-out, deactivate the remaining sockets
   if (nto == 0) {
      TList *al = mon->GetListOfActives();
      if (al && al->GetSize() > 0) {
         // Notify the name of those which did timeout
         Info("Collect"," %d node(s) went in timeout:", al->GetSize());
         TIter nxs(al);
         TSocket *xs = 0;
         while ((xs = (TSocket *)nxs())) {
            TSlave *wrk = FindSlave(xs);
            if (wrk)
               Info("Collect","   %s", wrk->GetName());
            else
               Info("Collect","   %p: %s:%d", xs, xs->GetInetAddress().GetHostName(),
                                                  xs->GetInetAddress().GetPort());
         }
      }
      mon->DeActivateAll();
   }

   // Deactivate Ctrl-C special handler
   if (fIntHandler)
      fIntHandler->Remove();

   // make sure group view is up to date
   SendGroupView();

   // Restore redirection setting
   fRedirLog = saveRedirLog;

   // Restore the monitor
   fCurrentMonitor = savedMonitor;

   ActivateAsyncInput();

   return cnt;
}

//______________________________________________________________________________
void TProof::CleanGDirectory(TList *ol)
{
   // Remove links to objects in list 'ol' from gDirectory

   if (ol) {
      TIter nxo(ol);
      TObject *o = 0;
      while ((o = nxo()))
         gDirectory->RecursiveRemove(o);
   }
}

//______________________________________________________________________________
Int_t TProof::CollectInputFrom(TSocket *s, Int_t endtype)
{
   // Collect and analyze available input from socket s.
   // Returns 0 on success, -1 if any failure occurs.

   TMessage *mess;

   Int_t recvrc = 0;
   if ((recvrc = s->Recv(mess)) < 0) {
      PDB(kGlobal,2)
         Info("CollectInputFrom","%p: got %d from Recv()", s, recvrc);
      Bool_t bad = kTRUE;
      if (recvrc == -5) {
         // Broken connection: try reconnection
         if (fCurrentMonitor) fCurrentMonitor->Remove(s);
         if (s->Reconnect() == 0) {
            if (fCurrentMonitor) fCurrentMonitor->Add(s);
            bad = kFALSE;
         }
      }
      if (bad)
         MarkBad(s, "problems receiving a message in TProof::CollectInputFrom(...)");
      // Ignore this wake up
      return -1;
   }
   if (!mess) {
      // we get here in case the remote server died
      MarkBad(s, "undefined message in TProof::CollectInputFrom(...)");
      return -1;
   }
   Int_t rc = 0;

   Int_t what = mess->What();
   TSlave *sl = FindSlave(s);
   rc = HandleInputMessage(sl, mess);
   if (rc == 1 && (endtype >= 0) && (what != endtype))
      // This message was for the base monitor in recursive case
      rc = 2;

   // We are done successfully
   return rc;
}

//______________________________________________________________________________
Int_t TProof::HandleInputMessage(TSlave *sl, TMessage *mess)
{
   // Analyze the received message.
   // Returns 0 on success (1 if this the last message from this socket), -1 if
   // any failure occurs.

   char str[512];
   TObject *obj;
   Int_t rc = 0;

   if (!mess || !sl) {
      Warning("HandleInputMessage", "given an empty message or undefined worker");
      return -1;
   }
   Bool_t delete_mess = kTRUE;
   TSocket *s = sl->GetSocket();
   if (!s) {
      Warning("HandleInputMessage", "worker socket is undefined");
      return -1;
   }

   // The message type
   Int_t what = mess->What();

   PDB(kGlobal,3)
      Info("HandleInputMessage", "got type %d from '%s'", what, (sl ? sl->GetOrdinal() : "undef"));

   switch (what) {

      case kMESS_OK:
         // Add the message to the list
         fRecvMessages->Add(mess);
         delete_mess = kFALSE;
         break;

      case kMESS_OBJECT:
         if (fPlayer) fPlayer->HandleRecvHisto(mess);
         break;

      case kPROOF_FATAL:
         MarkBad(s, "received kPROOF_FATAL");
         if (fProgressDialogStarted) {
            // Finalize the progress dialog
            Emit("StopProcess(Bool_t)", kTRUE);
         }
         break;

      case kPROOF_STOP:
         // Stop collection from this worker
         Info("HandleInputMessage", "received kPROOF_STOP from %s: disabling any further collection this worker",
                                    (sl ? sl->GetOrdinal() : "undef"));
         rc = 1;
         break;

      case kPROOF_GETTREEHEADER:
         // Add the message to the list
         fRecvMessages->Add(mess);
         delete_mess = kFALSE;
         rc = 1;
         break;

      case kPROOF_TOUCH:
         // send a request for touching the remote admin file
         {
            sl->Touch();
         }
         break;

      case kPROOF_GETOBJECT:
         // send slave object it asks for
         mess->ReadString(str, sizeof(str));
         obj = gDirectory->Get(str);
         if (obj)
            s->SendObject(obj);
         else
            s->Send(kMESS_NOTOK);
         break;

      case kPROOF_GETPACKET:
         {
            TDSetElement *elem = 0;
            elem = fPlayer ? fPlayer->GetNextPacket(sl, mess) : 0;

            if (elem != (TDSetElement*) -1) {
               TMessage answ(kPROOF_GETPACKET);
               answ << elem;
               s->Send(answ);

               while (fWaitingSlaves != 0 && fWaitingSlaves->GetSize()) {
                  TPair *p = (TPair*) fWaitingSlaves->First();
                  s = (TSocket*) p->Key();
                  TMessage *m = (TMessage*) p->Value();

                  elem = fPlayer->GetNextPacket(sl, m);
                  if (elem != (TDSetElement*) -1) {
                     TMessage a(kPROOF_GETPACKET);
                     a << elem;
                     s->Send(a);
                     // remove has to happen via Links because TPair does not have
                     // a Compare() function and therefore RemoveFirst() and
                     // Remove(TObject*) do not work
                     fWaitingSlaves->Remove(fWaitingSlaves->FirstLink());
                     delete p;
                     delete m;
                  } else {
                     break;
                  }
               }
            } else {
               if (fWaitingSlaves == 0) fWaitingSlaves = new TList;
               fWaitingSlaves->Add(new TPair(s, mess));
               delete_mess = kFALSE;
            }
         }
         break;

      case kPROOF_LOGFILE:
         {
            Int_t size;
            (*mess) >> size;
            PDB(kGlobal,2)
               Info("HandleInputMessage","kPROOF_LOGFILE: size: %d", size);
            RecvLogFile(s, size);
         }
         break;

      case kPROOF_LOGDONE:
         (*mess) >> sl->fStatus >> sl->fParallel;
         PDB(kGlobal,2)
            Info("HandleInputMessage","kPROOF_LOGDONE:%s: status %d  parallel %d",
                 sl->GetOrdinal(), sl->fStatus, sl->fParallel);
         if (sl->fStatus != 0) fStatus = sl->fStatus; //return last nonzero status
         rc = 1;
         break;

      case kPROOF_GETSTATS:
         {
            (*mess) >> sl->fBytesRead >> sl->fRealTime >> sl->fCpuTime
                  >> sl->fWorkDir >> sl->fProofWorkDir;
            TString img;
            if ((mess->BufferSize() > mess->Length()))
               (*mess) >> img;
            // Set image
            if (img.IsNull()) {
               if (sl->fImage.IsNull())
                  sl->fImage = Form("%s:%s", TUrl(sl->fName).GetHostFQDN(),
                                             sl->fProofWorkDir.Data());
            } else {
               sl->fImage = img;
            }
            PDB(kGlobal,2)
               Info("HandleInputMessage",
                        "kPROOF_GETSTATS:%s image: %s", sl->GetOrdinal(), sl->GetImage());

            fBytesRead += sl->fBytesRead;
            fRealTime  += sl->fRealTime;
            fCpuTime   += sl->fCpuTime;
            rc = 1;
         }
         break;

      case kPROOF_GETPARALLEL:
         {
            Bool_t async = kFALSE;
            (*mess) >> sl->fParallel;
            if ((mess->BufferSize() > mess->Length()))
               (*mess) >> async;
            rc = (async) ? 0 : 1;
         }
         break;

      case kPROOF_CHECKFILE:
         {  // New servers (>= 5.22) send the status
            if ((mess->BufferSize() > mess->Length())) {
               (*mess) >> fCheckFileStatus;
            } else {
               // Form old servers this meant success (failure was signaled with the
               // dangerous kPROOF_FATAL)
               fCheckFileStatus = 1;
            }
            rc = 1;
         }
         break;

      case kPROOF_SENDFILE:
         {  // New server: signals ending of sendfile operation
            rc = 1;
         }
         break;

      case kPROOF_PACKAGE_LIST:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_PACKAGE_LIST: enter");
            Int_t type = 0;
            (*mess) >> type;
            switch (type) {
            case TProof::kListEnabledPackages:
               SafeDelete(fEnabledPackages);
               fEnabledPackages = (TList *) mess->ReadObject(TList::Class());
               if (fEnabledPackages) {
                  fEnabledPackages->SetOwner();
               } else {
                  Error("HandleInputMessage",
                        "kPROOF_PACKAGE_LIST: kListEnabledPackages: TList not found in message!");
               }
               break;
            case TProof::kListPackages:
               SafeDelete(fAvailablePackages);
               fAvailablePackages = (TList *) mess->ReadObject(TList::Class());
               if (fAvailablePackages) {
                  fAvailablePackages->SetOwner();
               } else {
                  Error("HandleInputMessage",
                        "kPROOF_PACKAGE_LIST: kListPackages: TList not found in message!");
               }
               break;
            default:
               Error("HandleInputMessage", "kPROOF_PACKAGE_LIST: unknown type: %d", type);
            }
         }
         break;

      case kPROOF_OUTPUTOBJECT:
         {
            PDB(kGlobal,2)
               Info("HandleInputMessage","kPROOF_OUTPUTOBJECT: enter");
            Int_t type = 0;
            (*mess) >> type;
            // If a query result header, add it to the player list
            if (fPlayer) {
               if (type == 0) {
                  // Retrieve query result instance (output list not filled)
                  TQueryResult *pq =
                     (TQueryResult *) mess->ReadObject(TQueryResult::Class());
                  if (pq) {
                     // Add query to the result list in TProofPlayer
                     fPlayer->AddQueryResult(pq);
                     fPlayer->SetCurrentQuery(pq);
                     // And clear the output list, as we start merging a new set of results
                     if (fPlayer->GetOutputList())
                        fPlayer->GetOutputList()->Clear();
                     // Add the unique query tag as TNamed object to the input list
                     // so that it is available in TSelectors for monitoring
                     fPlayer->AddInput(new TNamed("PROOF_QueryTag",
                                       Form("%s:%s",pq->GetTitle(),pq->GetName())));
                  } else {
                     Warning("HandleInputMessage","kPROOF_OUTPUTOBJECT: query result missing");
                  }
               } else if (type > 0) {
                  // Read object
                  TObject *o = mess->ReadObject(TObject::Class());
                  // Add or merge it
                  if ((fPlayer->AddOutputObject(o) == 1)) {
                     // Remove the object if it has been merged
                     SafeDelete(o);
                  }
                  if (type > 1 && TestBit(TProof::kIsClient) && !IsLite()) {
                     // In PROOFLite this has to be done once only in TProofLite::Process
                     TQueryResult *pq = fPlayer->GetCurrentQuery();
                     pq->SetOutputList(fPlayer->GetOutputList(), kFALSE);
                     pq->SetInputList(fPlayer->GetInputList(), kFALSE);
                     // If the last object, notify the GUI that the result arrived
                     QueryResultReady(Form("%s:%s", pq->GetTitle(), pq->GetName()));
                     // Processing is over
                     UpdateDialog();
                  }
               }
            } else {
               Warning("HandleInputMessage", "kPROOF_OUTPUTOBJECT: player undefined!");
            }
         }
         break;

      case kPROOF_OUTPUTLIST:
         {
            PDB(kGlobal,2)
               Info("HandleInputMessage","kPROOF_OUTPUTLIST: enter");
            TList *out = 0;
            if (fPlayer) {
               if (TestBit(TProof::kIsMaster) || fProtocol < 7) {
                  out = (TList *) mess->ReadObject(TList::Class());
               } else {
                  TQueryResult *pq =
                     (TQueryResult *) mess->ReadObject(TQueryResult::Class());
                  if (pq) {
                     // Add query to the result list in TProofPlayer
                     fPlayer->AddQueryResult(pq);
                     fPlayer->SetCurrentQuery(pq);
                     // To avoid accidental cleanups from anywhere else
                     // remove objects from gDirectory and clone the list
                     out = pq->GetOutputList();
                     CleanGDirectory(out);
                     out = (TList *) out->Clone();
                     // Notify the GUI that the result arrived
                     QueryResultReady(Form("%s:%s", pq->GetTitle(), pq->GetName()));
                  } else {
                     PDB(kGlobal,2)
                        Info("HandleInputMessage","kPROOF_OUTPUTLIST: query result missing");
                  }
               }
               if (out) {
                  out->SetOwner();
                  fPlayer->AddOutput(out); // Incorporate the list
                  SafeDelete(out);
               } else {
                  PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_OUTPUTLIST: ouputlist is empty");
               }
            } else {
               Warning("HandleInputMessage", "kPROOF_OUTPUTLIST: player undefined!");
            }
            // On clients at this point processing is over
            if (TestBit(TProof::kIsClient) && !IsLite())
               UpdateDialog();
         }
         break;

      case kPROOF_QUERYLIST:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_QUERYLIST: enter");
            (*mess) >> fOtherQueries >> fDrawQueries;
            if (fQueries) {
               fQueries->Delete();
               delete fQueries;
               fQueries = 0;
            }
            fQueries = (TList *) mess->ReadObject(TList::Class());
         }
         break;

      case kPROOF_RETRIEVE:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_RETRIEVE: enter");
            TQueryResult *pq =
               (TQueryResult *) mess->ReadObject(TQueryResult::Class());
            if (pq && fPlayer) {
               fPlayer->AddQueryResult(pq);
               // Notify the GUI that the result arrived
               QueryResultReady(Form("%s:%s", pq->GetTitle(), pq->GetName()));
            } else {
               PDB(kGlobal,2)
                  Info("HandleInputMessage","kPROOF_RETRIEVE: query result missing or player undefined");
            }
         }
         break;

      case kPROOF_MAXQUERIES:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_MAXQUERIES: enter");
            Int_t max = 0;

            (*mess) >> max;
            Printf("Number of queries fully kept remotely: %d", max);
         }
         break;

      case kPROOF_SERVERSTARTED:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_SERVERSTARTED: enter");

            UInt_t tot = 0, done = 0;
            TString action;
            Bool_t st = kTRUE;

            (*mess) >> action >> tot >> done >> st;

            if (TestBit(TProof::kIsClient)) {
               if (tot) {
                  TString type = (action.Contains("submas")) ? "submasters"
                                                             : "workers";
                  Int_t frac = (Int_t) (done*100.)/tot;
                  char msg[512] = {0};
                  if (frac >= 100) {
                     sprintf(msg,"%s: OK (%d %s)                 \n",
                             action.Data(),tot, type.Data());
                  } else {
                     sprintf(msg,"%s: %d out of %d (%d %%)\r",
                             action.Data(), done, tot, frac);
                  }
                  if (fSync)
                     fprintf(stderr,"%s", msg);
                  else
                     NotifyLogMsg(msg, 0);
               }
               // Notify GUIs
               StartupMessage(action.Data(), st, (Int_t)done, (Int_t)tot);
            } else {

               // Just send the message one level up
               TMessage m(kPROOF_SERVERSTARTED);
               m << action << tot << done << st;
               gProofServ->GetSocket()->Send(m);
            }
         }
         break;

      case kPROOF_DATASET_STATUS:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_DATASET_STATUS: enter");

            UInt_t tot = 0, done = 0;
            TString action;
            Bool_t st = kTRUE;

            (*mess) >> action >> tot >> done >> st;

            if (TestBit(TProof::kIsClient)) {
               if (tot) {
                  TString type = "files";
                  Int_t frac = (Int_t) (done*100.)/tot;
                  char msg[512] = {0};
                  if (frac >= 100) {
                     sprintf(msg,"%s: OK (%d %s)                 \n",
                             action.Data(),tot, type.Data());
                  } else {
                     sprintf(msg,"%s: %d out of %d (%d %%)\r",
                             action.Data(), done, tot, frac);
                  }
                  if (fSync)
                     fprintf(stderr,"%s", msg);
                  else
                     NotifyLogMsg(msg, 0);
               }
               // Notify GUIs
               DataSetStatus(action.Data(), st, (Int_t)done, (Int_t)tot);
            } else {

               // Just send the message one level up
               TMessage m(kPROOF_DATASET_STATUS);
               m << action << tot << done << st;
               gProofServ->GetSocket()->Send(m);
            }
         }
         break;

      case kPROOF_STARTPROCESS:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_STARTPROCESS: enter");

            // For Proof-Lite this variable is the number of workers and is set
            // by the player
            if (!IsLite()) {
               fNotIdle = 1;
               fIsWaiting = kFALSE;
            }

            // The signal is used on masters by XrdProofdProtocol to catch
            // the start of processing; on clients it allows to update the
            // progress dialog
            if (!TestBit(TProof::kIsMaster)) {
               TString selec;
               Int_t dsz = -1;
               Long64_t first = -1, nent = -1;
               (*mess) >> selec >> dsz >> first >> nent;

               // Start or reset the progress dialog
               if (!gROOT->IsBatch()) {
                  if (fProgressDialog && !TestBit(kUsingSessionGui)) {
                     if (!fProgressDialogStarted) {
                        fProgressDialog->ExecPlugin(5, this,
                                                   selec.Data(), dsz, first, nent);
                        fProgressDialogStarted = kTRUE;
                     } else {
                        ResetProgressDialog(selec, dsz, first, nent);
                     }
                  }
                  ResetBit(kUsingSessionGui);
               }
            }
         }
         break;

      case kPROOF_ENDINIT:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_ENDINIT: enter");

            if (TestBit(TProof::kIsMaster)) {
               if (fPlayer)
                  fPlayer->SetInitTime();
            }
         }
         break;

      case kPROOF_SETIDLE:
         {
            PDB(kGlobal,2)
               Info("HandleInputMessage","kPROOF_SETIDLE: enter");

            // The session is idle
            if (IsLite()) {
               if (fNotIdle > 0) {
                  fNotIdle--;
               } else {
                  Warning("HandleInputMessage", "got kPROOF_SETIDLE but no running workers ! protocol error?");
               }
            } else {
               fNotIdle = 0;
               // Check if the query has been enqueued
               if ((mess->BufferSize() > mess->Length()))
                  (*mess) >> fIsWaiting;
            }
         }
         break;

      case kPROOF_QUERYSUBMITTED:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_QUERYSUBMITTED: enter");

            // We have received the sequential number
            (*mess) >> fSeqNum;
            Bool_t sync = fSync;
            if ((mess->BufferSize() > mess->Length()))
               (*mess) >> sync;
            if (sync !=  fSync && fSync) {
               // The server required to switch to asynchronous mode
               Activate();
               fSync = kFALSE;
            }
            // Check if the query has been enqueued
            fIsWaiting = kTRUE;
            // For Proof-Lite this variable is the number of workers and is set by the player
            if (!IsLite())
               fNotIdle = 1;

            rc = 1;
         }
         break;

      case kPROOF_SESSIONTAG:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_SESSIONTAG: enter");

            // We have received the unique tag and save it as name of this object
            TString stag;
            (*mess) >> stag;
            SetName(stag);
         }
         break;

      case kPROOF_FEEDBACK:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_FEEDBACK: enter");
            TList *out = (TList *) mess->ReadObject(TList::Class());
            out->SetOwner();
            if (fPlayer)
               fPlayer->StoreFeedback(sl, out); // Adopts the list
            else
               // Not yet ready: stop collect asap
               rc = 1;
         }
         break;

      case kPROOF_AUTOBIN:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_AUTOBIN: enter");

            TString name;
            Double_t xmin, xmax, ymin, ymax, zmin, zmax;

            (*mess) >> name >> xmin >> xmax >> ymin >> ymax >> zmin >> zmax;

            if (fPlayer) fPlayer->UpdateAutoBin(name,xmin,xmax,ymin,ymax,zmin,zmax);

            TMessage answ(kPROOF_AUTOBIN);

            answ << name << xmin << xmax << ymin << ymax << zmin << zmax;

            s->Send(answ);
         }
         break;

      case kPROOF_PROGRESS:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_PROGRESS: enter");

            if (GetRemoteProtocol() > 11) {
               // New format
               Long64_t total, processed, bytesread;
               Float_t initTime, procTime, evtrti, mbrti;
               (*mess) >> total >> processed >> bytesread
                       >> initTime >> procTime
                       >> evtrti >> mbrti;
               if (fPlayer)
                  fPlayer->Progress(sl, total, processed, bytesread,
                                    initTime, procTime, evtrti, mbrti);

            } else {
               // Old format
               Long64_t total, processed;
               (*mess) >> total >> processed;
               if (fPlayer)
                  fPlayer->Progress(sl, total, processed);
            }
         }
         break;

      case kPROOF_STOPPROCESS:
         {
            // This message is sent from a worker that finished processing.
            // We determine whether it was asked to finish by the
            // packetizer or stopped during processing a packet
            // (by TProof::RemoveWorkers() or by an external signal).
            // In the later case call packetizer->MarkBad.
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_STOPPROCESS: enter");

            Long64_t events = 0;
            Bool_t abort = kFALSE;
            TProofProgressStatus *status = 0;

            if ((mess->BufferSize() > mess->Length()) && (fProtocol > 18)) {
               (*mess) >> status >> abort;
            } else if ((mess->BufferSize() > mess->Length()) && (fProtocol > 8)) {
               (*mess) >> events >> abort;
            } else {
               (*mess) >> events;
            }
            if (!abort && fPlayer) {
               if (fProtocol > 18) {
                  TList *listOfMissingFiles = 0;
                  if (!(listOfMissingFiles = (TList *)GetOutput("MissingFiles"))) {
                     listOfMissingFiles = new TList();
                     listOfMissingFiles->SetName("MissingFiles");
                     if (fPlayer)
                        fPlayer->AddOutputObject(listOfMissingFiles);
                  }
                  if (fPlayer->GetPacketizer()) {
                     Int_t ret =
                        fPlayer->GetPacketizer()->AddProcessed(sl, status, 0, &listOfMissingFiles);
                     if (ret > 0)
                        fPlayer->GetPacketizer()->MarkBad(sl, status, &listOfMissingFiles);
                  }
               } else {
                  fPlayer->AddEventsProcessed(events);
               }
            }
            SafeDelete(status);
            if (!TestBit(TProof::kIsMaster))
               Emit("StopProcess(Bool_t)", abort);
            break;
         }

      case kPROOF_GETSLAVEINFO:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_GETSLAVEINFO: enter");

            Bool_t active = (GetListOfActiveSlaves()->FindObject(sl) != 0);
            Bool_t bad = (GetListOfBadSlaves()->FindObject(sl) != 0);
            TList* tmpinfo = 0;
            (*mess) >> tmpinfo;
            if (tmpinfo == 0) {
               Error("HandleInputMessage","kPROOF_GETSLAVEINFO: no list received!");
            } else {
               tmpinfo->SetOwner(kFALSE);
               Int_t nentries = tmpinfo->GetSize();
               for (Int_t i=0; i<nentries; i++) {
                  TSlaveInfo* slinfo =
                     dynamic_cast<TSlaveInfo*>(tmpinfo->At(i));
                  if (slinfo) {
                     fSlaveInfo->Add(slinfo);
                     if (slinfo->fStatus != TSlaveInfo::kBad) {
                        if (!active) slinfo->SetStatus(TSlaveInfo::kNotActive);
                        if (bad) slinfo->SetStatus(TSlaveInfo::kBad);
                     }
                     if (!sl->GetMsd().IsNull()) slinfo->fMsd = sl->GetMsd();
                  }
               }
               delete tmpinfo;
               rc = 1;
            }
         }
         break;

      case kPROOF_VALIDATE_DSET:
         {
            PDB(kGlobal,2)
               Info("HandleInputMessage","kPROOF_VALIDATE_DSET: enter");
            TDSet* dset = 0;
            (*mess) >> dset;
            if (!fDSet)
               Error("HandleInputMessage","kPROOF_VALIDATE_DSET: fDSet not set");
            else
               fDSet->Validate(dset);
            delete dset;
         }
         break;

      case kPROOF_DATA_READY:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_DATA_READY: enter");
            Bool_t dataready = kFALSE;
            Long64_t totalbytes, bytesready;
            (*mess) >> dataready >> totalbytes >> bytesready;
            fTotalBytes += totalbytes;
            fBytesReady += bytesready;
            if (dataready == kFALSE) fDataReady = dataready;
         }
         break;

      case kPROOF_PING:
         // do nothing (ping is already acknowledged)
         break;

      case kPROOF_MESSAGE:
         {
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_MESSAGE: enter");

            // We have received the unique tag and save it as name of this object
            TString msg;
            (*mess) >> msg;
            Bool_t lfeed = kTRUE;
            if ((mess->BufferSize() > mess->Length()))
               (*mess) >> lfeed;

            if (TestBit(TProof::kIsClient)) {

               if (fSync) {
                  // Notify locally
                  fprintf(stderr,"%s%c", msg.Data(), (lfeed ? '\n' : '\r'));
               } else {
                  // Notify locally taking care of redirection, windows logs, ...
                  NotifyLogMsg(msg, (lfeed ? "\n" : "\r"));
               }
            } else {

               // The message is logged for debugging purposes.
               fprintf(stderr,"%s%c", msg.Data(), (lfeed ? '\n' : '\r'));
               if (gProofServ) {
                  // We hide it during normal operations
                  gProofServ->FlushLogFile();

                  // And send the message one level up
                  gProofServ->SendAsynMessage(msg, lfeed);
               }
            }
         }
         break;

      case kPROOF_VERSARCHCOMP:
         {
            TString vac;
            (*mess) >> vac;
            PDB(kGlobal,2) Info("HandleInputMessage","kPROOF_VERSARCHCOMP: %s", vac.Data());
            Int_t from = 0;
            TString vers, archcomp;
            if (vac.Tokenize(vers, from, "|"))
               vac.Tokenize(archcomp, from, "|");
            sl->SetArchCompiler(archcomp);
            vers.ReplaceAll(":","|");
            sl->SetROOTVersion(vers);
         }
         break;

      default:
         {
            Error("HandleInputMessage", "unknown command received from '%s' (what = %d)",
                                      (sl ? sl->GetOrdinal() : "undef"), what);
         }
         break;
   }

   // Cleanup
   if (delete_mess)
      delete mess;

   // We are done successfully
   return rc;
}

//______________________________________________________________________________
void TProof::UpdateDialog()
{
   // Final update of the progress dialog

   if (!fPlayer) return;

   // Handle abort ...
   if (fPlayer->GetExitStatus() == TVirtualProofPlayer::kAborted) {
      if (fSync)
         Info("UpdateDialog",
              "processing was aborted - %lld events processed",
              fPlayer->GetEventsProcessed());

      if (GetRemoteProtocol() > 11) {
         // New format
         Progress(-1, fPlayer->GetEventsProcessed(), -1, -1., -1., -1., -1.);
      } else {
         Progress(-1, fPlayer->GetEventsProcessed());
      }
      Emit("StopProcess(Bool_t)", kTRUE);
   }

   // Handle stop ...
   if (fPlayer->GetExitStatus() == TVirtualProofPlayer::kStopped) {
      if (fSync)
         Info("UpdateDialog",
              "processing was stopped - %lld events processed",
              fPlayer->GetEventsProcessed());

      if (GetRemoteProtocol() > 11) {
         // New format
         Progress(-1, fPlayer->GetEventsProcessed(), -1, -1., -1., -1., -1.);
      } else {
         Progress(-1, fPlayer->GetEventsProcessed());
      }
      Emit("StopProcess(Bool_t)", kFALSE);
   }

   // Final update of the dialog box
   if (GetRemoteProtocol() > 11) {
      // New format
      EmitVA("Progress(Long64_t,Long64_t,Long64_t,Float_t,Float_t,Float_t,Float_t)",
               7, (Long64_t)(-1), (Long64_t)(-1), (Long64_t)(-1),
                  (Float_t)(-1.),(Float_t)(-1.),(Float_t)(-1.),(Float_t)(-1.));
   } else {
      EmitVA("Progress(Long64_t,Long64_t)", 2, (Long64_t)(-1), (Long64_t)(-1));
   }
}

//______________________________________________________________________________
void TProof::ActivateAsyncInput()
{
   // Activate the a-sync input handler.

   TIter next(fSlaves);
   TSlave *sl;

   while ((sl = (TSlave*) next()))
      if (sl->GetInputHandler())
         sl->GetInputHandler()->Add();
}

//______________________________________________________________________________
void TProof::DeActivateAsyncInput()
{
   // De-actiate a-sync input handler.

   TIter next(fSlaves);
   TSlave *sl;

   while ((sl = (TSlave*) next()))
      if (sl->GetInputHandler())
         sl->GetInputHandler()->Remove();
}

//______________________________________________________________________________
void TProof::MarkBad(TSlave *wrk, const char *reason)
{
   // Add a bad slave server to the bad slave list and remove it from
   // the active list and from the two monitor objects. Assume that the work
   // done by this worker was lost and ask packerizer to reassign it.

   R__LOCKGUARD2(fCloseMutex);


   // We may have been invalidated in the meanwhile: nothing to do in such a case
   if (!IsValid()) return;

   if (!wrk) {
      Error("MarkBad", "worker instance undefined: protocol error? ");
      return;
   }

   // Local URL
   static TString thisurl;
   if (thisurl.IsNull()) {
      if (IsMaster()) {
         Int_t port = gEnv->GetValue("ProofServ.XpdPort",-1);
         thisurl = (port > 0) ? Form("%s:%d", TUrl(gSystem->HostName()).GetHostFQDN(), port)
                              : TUrl(gSystem->HostName()).GetHostFQDN();
      } else {
         thisurl = Form("%s@%s:%d", fUrl.GetUser(), fUrl.GetHost(), fUrl.GetPort());
      }
   }

   if (!reason || strcmp(reason, kPROOF_TerminateWorker)) {
      // Message for notification
      const char *mastertype = (gProofServ && gProofServ->IsTopMaster()) ? "top master" : "master";
      TString src = IsMaster() ? Form("%s at %s", mastertype, thisurl.Data()) : "local session";
      TString msg(Form("\n +++ Message from %s : ", src.Data()));
      msg += Form("marking %s:%d (%s) as bad\n +++ Reason: %s",
                  wrk->GetName(), wrk->GetPort(), wrk->GetOrdinal(),
                  (reason && strlen(reason)) ? reason : "unknown");
      Info("MarkBad", "%s", msg.Data());
      // Notify one level up, if the case
      // Add some hint for diagnostics
      if (gProofServ) {
         msg += Form("\n\n +++ Most likely your code crashed on worker %s at %s:%d.\n",
                     wrk->GetOrdinal(), wrk->GetName(), wrk->GetPort());
      } else {
         msg = Form("\n\n +++ Most likely your code crashed\n");
      }
      msg += Form(" +++ Please check the session logs for error messages either using\n");
      msg += Form(" +++ the 'Show logs' button or executing\n");
      msg += Form(" +++\n");
      if (gProofServ) {
         msg += Form(" +++ root [] TProof::Mgr(\"%s\")->GetSessionLogs()->Display(\"%s\",0)\n\n",
                     thisurl.Data(), wrk->GetOrdinal());
         gProofServ->SendAsynMessage(msg, kTRUE);
      } else {
         msg += Form(" +++ root [] TProof::Mgr(\"%s\")->GetSessionLogs()->Display(\"*\")\n\n",
                     thisurl.Data());
         Printf("%s", msg.Data());
      }
   } else if (reason) {
      if (gDebug > 0) {
         Info("MarkBad", "worker %s at %s:%d asked to terminate",
                         wrk->GetOrdinal(), wrk->GetName(), wrk->GetPort());
      }
   }

   if (IsMaster() && reason) {
      if (strcmp(reason, kPROOF_TerminateWorker)) {
         // if the reason was not a planned termination
         TList *listOfMissingFiles = 0;
         if (!(listOfMissingFiles = (TList *)GetOutput("MissingFiles"))) {
            listOfMissingFiles = new TList();
            listOfMissingFiles->SetName("MissingFiles");
            if (fPlayer)
               fPlayer->AddOutputObject(listOfMissingFiles);
         }
         // If a query is being processed, assume that the work done by
         // the worker was lost and needs to be reassigned.
         TVirtualPacketizer *packetizer = fPlayer ? fPlayer->GetPacketizer() : 0;
         if (packetizer) {
            // the worker was lost so do resubmit the packets
            packetizer->MarkBad(wrk, 0, &listOfMissingFiles);
         }
      } else {
         // Tell the coordinator that we are gone
         if (gProofServ) {
            TString ord(wrk->GetOrdinal());
            Int_t id = ord.Last('.');
            if (id != kNPOS) ord.Remove(0, id+1);
            gProofServ->ReleaseWorker(ord.Data());
         }
      }
   }

   fActiveSlaves->Remove(wrk);
   FindUniqueSlaves();

   fAllMonitor->Remove(wrk->GetSocket());
   fActiveMonitor->Remove(wrk->GetSocket());

   fSendGroupView = kTRUE;

   if (IsMaster()) {
      if (reason && !strcmp(reason, kPROOF_TerminateWorker)) {
         // if the reason was a planned termination then delete the worker and
         // remove it from all the lists
         fSlaves->Remove(wrk);
         fBadSlaves->Remove(wrk);
         fActiveSlaves->Remove(wrk);
         fInactiveSlaves->Remove(wrk);
         fUniqueSlaves->Remove(wrk);
         fAllUniqueSlaves->Remove(wrk);
         fNonUniqueMasters->Remove(wrk);
         delete wrk;
      } else {
         fBadSlaves->Add(wrk);
         wrk->Close();
      }

      // Update session workers files
      SaveWorkerInfo();
   } else {
      // On clients the proof session should be removed from the lists
      // and deleted, since it is not valid anymore
      fSlaves->Remove(wrk);
      if (fManager)
         fManager->ShutdownSession(this);
   }
}

//______________________________________________________________________________
void TProof::MarkBad(TSocket *s, const char *reason)
{
   // Add slave with socket s to the bad slave list and remove if from
   // the active list and from the two monitor objects.

   R__LOCKGUARD2(fCloseMutex);

   // We may have been invalidated in the meanwhile: nothing to do in such a case
   if (!IsValid()) return;

   TSlave *wrk = FindSlave(s);
   MarkBad(wrk, reason);
}

//______________________________________________________________________________
void TProof::TerminateWorker(TSlave *wrk)
{
   // Ask an active worker 'wrk' to terminate, i.e. to shutdown

   if (!wrk) {
      Warning("TerminateWorker", "worker instance undefined: protocol error? ");
      return;
   }

   // Send stop message
   if (wrk->GetSocket() && wrk->GetSocket()->IsValid()) {
      TMessage mess(kPROOF_STOP);
      wrk->GetSocket()->Send(mess);
   } else {
      if (gDebug > 0)
         Info("TerminateWorker", "connection to worker is already down: cannot"
                                 " send termination message");
   }

   // This is a bad worker from now on
   MarkBad(wrk, kPROOF_TerminateWorker);
}

//______________________________________________________________________________
void TProof::TerminateWorker(const char *ord)
{
   // Ask an active worker 'ord' to terminate, i.e. to shutdown

   if (ord && strlen(ord) > 0) {
      Bool_t all = (ord[0] == '*') ? kTRUE : kFALSE;
      if (IsMaster()) {
         TIter nxw(fSlaves);
         TSlave *wrk = 0;
         while ((wrk = (TSlave *)nxw())) {
            if (all || !strcmp(wrk->GetOrdinal(), ord)) {
               TerminateWorker(wrk);
               if (!all) break;
            }
         }
      } else {
         TMessage mess(kPROOF_STOP);
         mess << TString(ord);
         Broadcast(mess);
      }
   }
}

//______________________________________________________________________________
Int_t TProof::Ping()
{
   // Ping PROOF. Returns 1 if master server responded.

   return Ping(kActive);
}

//______________________________________________________________________________
Int_t TProof::Ping(ESlaves list)
{
   // Ping PROOF slaves. Returns the number of slaves that responded.

   TList *slaves = 0;
   if (list == kAll)       slaves = fSlaves;
   if (list == kActive)    slaves = fActiveSlaves;
   if (list == kUnique)    slaves = fUniqueSlaves;
   if (list == kAllUnique) slaves = fAllUniqueSlaves;

   if (slaves->GetSize() == 0) return 0;

   int   nsent = 0;
   TIter next(slaves);

   TSlave *sl;
   while ((sl = (TSlave *)next())) {
      if (sl->IsValid()) {
         if (sl->Ping() == -1) {
            MarkBad(sl, "ping unsuccessful");
         } else {
            nsent++;
         }
      }
   }

   return nsent;
}

//______________________________________________________________________________
void TProof::Touch()
{
   // Ping PROOF slaves. Returns the number of slaves that responded.

   TList *slaves = fSlaves;

   if (slaves->GetSize() == 0) return;

   TIter next(slaves);

   TSlave *sl;
   while ((sl = (TSlave *)next())) {
      if (sl->IsValid()) {
         sl->Touch();
      }
   }

   return;
}

//______________________________________________________________________________
void TProof::Print(Option_t *option) const
{
   // Print status of PROOF cluster.

   TString secCont;

   if (TestBit(TProof::kIsClient)) {
      Printf("Connected to:             %s (%s)", GetMaster(),
                                             IsValid() ? "valid" : "invalid");
      Printf("Port number:              %d", GetPort());
      Printf("User:                     %s", GetUser());
      if (gROOT->GetSvnRevision() > 0)
         Printf("ROOT version|rev:         %s|r%d", gROOT->GetVersion(), gROOT->GetSvnRevision());
      else
         Printf("ROOT version:             %s", gROOT->GetVersion());
      Printf("Architecture-Compiler:    %s-%s", gSystem->GetBuildArch(),
                                                gSystem->GetBuildCompilerVersion());
      TSlave *sl = (TSlave *)fActiveSlaves->First();
      if (sl) {
         TString sc;
         if (sl->GetSocket()->GetSecContext())
            Printf("Security context:         %s",
                                      sl->GetSocket()->GetSecContext()->AsString(sc));
         Printf("Proofd protocol version:  %d", sl->GetSocket()->GetRemoteProtocol());
      } else {
         Printf("Security context:         Error - No connection");
         Printf("Proofd protocol version:  Error - No connection");
      }
      Printf("Client protocol version:  %d", GetClientProtocol());
      Printf("Remote protocol version:  %d", GetRemoteProtocol());
      Printf("Log level:                %d", GetLogLevel());
      Printf("Session unique tag:       %s", IsValid() ? GetSessionTag() : "");
      Printf("Default data pool:        %s", IsValid() ? GetDataPoolUrl() : "");
      if (IsValid())
         const_cast<TProof*>(this)->SendPrint(option);
   } else {
      const_cast<TProof*>(this)->AskStatistics();
      if (IsParallel())
         Printf("*** Master server %s (parallel mode, %d workers):",
                gProofServ->GetOrdinal(), GetParallel());
      else
         Printf("*** Master server %s (sequential mode):",
                gProofServ->GetOrdinal());

      Printf("Master host name:           %s", gSystem->HostName());
      Printf("Port number:                %d", GetPort());
      if (strlen(gProofServ->GetGroup()) > 0) {
         Printf("User/Group:                 %s/%s", GetUser(), gProofServ->GetGroup());
      } else {
         Printf("User:                       %s", GetUser());
      }
      TString ver(gROOT->GetVersion());
      if (gROOT->GetSvnRevision() > 0)
         ver += Form("|r%d", gROOT->GetSvnRevision());
      if (gSystem->Getenv("ROOTVERSIONTAG"))
         ver += Form("|%s", gSystem->Getenv("ROOTVERSIONTAG"));
      Printf("ROOT version|rev|tag:       %s", ver.Data());
      Printf("Architecture-Compiler:      %s-%s", gSystem->GetBuildArch(),
                                                  gSystem->GetBuildCompilerVersion());
      Printf("Protocol version:           %d", GetClientProtocol());
      Printf("Image name:                 %s", GetImage());
      Printf("Working directory:          %s", gSystem->WorkingDirectory());
      Printf("Config directory:           %s", GetConfDir());
      Printf("Config file:                %s", GetConfFile());
      Printf("Log level:                  %d", GetLogLevel());
      Printf("Number of workers:          %d", GetNumberOfSlaves());
      Printf("Number of active workers:   %d", GetNumberOfActiveSlaves());
      Printf("Number of unique workers:   %d", GetNumberOfUniqueSlaves());
      Printf("Number of inactive workers: %d", GetNumberOfInactiveSlaves());
      Printf("Number of bad workers:      %d", GetNumberOfBadSlaves());
      Printf("Total MB's processed:       %.2f", float(GetBytesRead())/(1024*1024));
      Printf("Total real time used (s):   %.3f", GetRealTime());
      Printf("Total CPU time used (s):    %.3f", GetCpuTime());
      if (TString(option).Contains("a", TString::kIgnoreCase) && GetNumberOfSlaves()) {
         Printf("List of workers:");
         TList masters;
         TIter nextslave(fSlaves);
         while (TSlave* sl = dynamic_cast<TSlave*>(nextslave())) {
            if (!sl->IsValid()) continue;

            if (sl->GetSlaveType() == TSlave::kSlave) {
               sl->Print(option);
            } else if (sl->GetSlaveType() == TSlave::kMaster) {
               TMessage mess(kPROOF_PRINT);
               mess.WriteString(option);
               if (sl->GetSocket()->Send(mess) == -1)
                  const_cast<TProof*>(this)->MarkBad(sl, "could not send kPROOF_PRINT request");
               else
                  masters.Add(sl);
            } else {
               Error("Print", "TSlave is neither Master nor Worker");
               R__ASSERT(0);
            }
         }
         const_cast<TProof*>(this)->Collect(&masters, fCollectTimeout);
      }
   }
}

//______________________________________________________________________________
Long64_t TProof::Process(TDSet *dset, const char *selector, Option_t *option,
                         Long64_t nentries, Long64_t first)
{
   // Process a data set (TDSet) using the specified selector (.C) file.
   // Entry- or event-lists should be set in the data set object using
   // TDSet::SetEntryList.
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (!IsValid() || !fPlayer) return -1;

   // Set PROOF to running state
   SetRunStatus(TProof::kRunning);

   // Resolve query mode
   fSync = (GetQueryMode(option) == kSync);

   TString opt(option);
   if (fSync && (!IsIdle() || IsWaiting())) {
      // Already queued or processing queries: switch to asynchronous mode
      Info("Process", "session is in waiting or processing status: switch to asynchronous mode");
      fSync = kFALSE;
      opt.ReplaceAll("SYNC","");
      opt += "ASYN";
   }

   // Cleanup old temporary datasets
   if ((IsIdle() && !IsWaiting()) && fRunningDSets && fRunningDSets->GetSize() > 0) {
      fRunningDSets->SetOwner(kTRUE);
      fRunningDSets->Delete();
   }

   // deactivate the default application interrupt handler
   // ctrl-c's will be forwarded to PROOF to stop the processing
   TSignalHandler *sh = 0;
   if (fSync) {
      if (gApplication)
         sh = gSystem->RemoveSignalHandler(gApplication->GetSignalHandler());
   }

   Long64_t rv = fPlayer->Process(dset, selector, opt.Data(), nentries, first);

   if (fSync) {
      // reactivate the default application interrupt handler
      if (sh)
         gSystem->AddSignalHandler(sh);
   }

   return rv;
}

//______________________________________________________________________________
Long64_t TProof::Process(TFileCollection *fc, const char *selector,
                         Option_t *option, Long64_t nentries, Long64_t first)
{
   // Process a data set (TFileCollection) using the specified selector (.C) file.
   // The default tree is analyzed (i.e. the first one found). To specify another
   // tree, the default tree can be changed using TFileCollection::SetDefaultMetaData .
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (!IsValid() || !fPlayer) return -1;

   if (fProtocol < 17) {
      Info("Process", "server version < 5.18/00:"
                      " processing of TFileCollection not supported");
      return -1;
   }

   // We include the TFileCollection to the input list and we create a
   // fake TDSet with infor about it
   TDSet *dset = new TDSet(Form("TFileCollection:%s", fc->GetName()), 0, 0, "");
   fPlayer->AddInput(fc);
   Long64_t retval = Process(dset, selector, option, nentries, first);
   fPlayer->GetInputList()->Remove(fc); // To avoid problems in future

   // Cleanup
   if (IsLite() && !fSync) {
      if (!fRunningDSets) fRunningDSets = new TList;
      fRunningDSets->Add(dset);
   } else {
      delete dset;
   }

   return retval;
}

//______________________________________________________________________________
Long64_t TProof::Process(const char *dsetname, const char *selector,
                         Option_t *option, Long64_t nentries,
                         Long64_t first, TObject *enl)
{
   // Process a dataset which is stored on the master with name 'dsetname'.
   // The syntax for dsetname is name[#[dir/]objname], e.g.
   //   "mydset"       analysis of the first tree in the top dir of the dataset
   //                  named "mydset"
   //   "mydset#T"     analysis tree "T" in the top dir of the dataset
   //                  named "mydset"
   //   "mydset#adir/T" analysis tree "T" in the dir "adir" of the dataset
   //                  named "mydset"
   //   "mydset#adir/" analysis of the first tree in the dir "adir" of the
   //                  dataset named "mydset"
   // The last argument 'enl' specifies an entry- or event-list to be used as
   // event selection.
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (fProtocol < 13) {
      Info("Process", "processing 'by name' not supported by the server");
      return -1;
   }

   TString name(dsetname);
   TString obj;
   TString dir = "/";
   Int_t idxc = name.Index("#");
   if (idxc != kNPOS) {
      Int_t idxs = name.Index("/", 1, idxc, TString::kExact);
      if (idxs != kNPOS) {
         obj = name(idxs+1, name.Length());
         dir = name(idxc+1, name.Length());
         dir.Remove(dir.Index("/") + 1);
         name.Remove(idxc);
      } else {
         obj = name(idxc+1, name.Length());
         name.Remove(idxc);
      }
   } else if (name.Index(":") != kNPOS && name.Index("://") == kNPOS) {
      // protection against using ':' instead of '#'
      Error("Process", "bad name syntax (%s): please use"
                       " a '#' after the dataset name", dsetname);
      return -1;
   }

   TDSet *dset = new TDSet(name, obj, dir);
   // Set entry list
   dset->SetEntryList(enl);
   Long64_t retval = Process(dset, selector, option, nentries, first);
   // Cleanup
   if (IsLite() && !fSync) {
      if (!fRunningDSets) fRunningDSets = new TList;
      fRunningDSets->Add(dset);
   } else {
      delete dset;
   }
   return retval;
}

//______________________________________________________________________________
Long64_t TProof::Process(const char *selector, Long64_t n, Option_t *option)
{
   // Generic (non-data based) selector processing: the Process() method of the
   // specified selector (.C) is called 'n' times.
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (!IsValid()) return -1;

   if (fProtocol < 16) {
      Info("Process", "server version < 5.17/04: generic processing not supported");
      return -1;
   }

   // Fake data set
   TDSet *dset = new TDSet;
   dset->SetBit(TDSet::kEmpty);

   Long64_t retval = Process(dset, selector, option, n);

   // Cleanup
   if (IsLite() && !fSync) {
      if (!fRunningDSets) fRunningDSets = new TList;
      fRunningDSets->Add(dset);
   } else {
      delete dset;
   }
   return retval;
}

//______________________________________________________________________________
Int_t TProof::GetQueryReference(Int_t qry, TString &ref)
{
   // Get reference for the qry-th query in fQueries (as
   // displayed by ShowQueries).

   ref = "";
   if (qry > 0) {
      if (!fQueries)
         GetListOfQueries();
      if (fQueries) {
         TIter nxq(fQueries);
         TQueryResult *qr = 0;
         while ((qr = (TQueryResult *) nxq()))
            if (qr->GetSeqNum() == qry) {
               ref = Form("%s:%s", qr->GetTitle(), qr->GetName());
               return 0;
            }
      }
   }
   return -1;
}

//______________________________________________________________________________
Long64_t TProof::Finalize(Int_t qry, Bool_t force)
{
   // Finalize the qry-th query in fQueries.
   // If force, force retrieval if the query is found in the local list
   // but has already been finalized (default kFALSE).
   // If query < 0, finalize current query.
   // Return 0 on success, -1 on error

   if (fPlayer) {
      if (qry > 0) {
         TString ref;
         if (GetQueryReference(qry, ref) == 0) {
            return Finalize(ref, force);
         } else {
            Info("Finalize", "query #%d not found", qry);
         }
      } else {
         // The last query
         return Finalize("", force);
      }
   }
   return -1;
}

//______________________________________________________________________________
Long64_t TProof::Finalize(const char *ref, Bool_t force)
{
   // Finalize query with reference ref.
   // If force, force retrieval if the query is found in the local list
   // but has already been finalized (default kFALSE).
   // If ref = 0, finalize current query.
   // Return 0 on success, -1 on error

   if (fPlayer) {
      // Get the pointer to the query
      TQueryResult *qr = (ref && strlen(ref) > 0) ? fPlayer->GetQueryResult(ref)
                                                  : GetQueryResult();
      Bool_t retrieve = kFALSE;
      TString xref(ref);
      if (!qr) {
         if (!xref.IsNull()) {
            retrieve =  kTRUE;
         }
      } else {
         if (qr->IsFinalized()) {
            if (force) {
               retrieve = kTRUE;
            } else {
               Info("Finalize","query already finalized:"
                     " use Finalize(<qry>,kTRUE) to force new retrieval");
               qr = 0;
            }
         } else {
            retrieve = kTRUE;
            xref.Form("%s:%s", qr->GetTitle(), qr->GetName());
         }
      }
      if (retrieve) {
         Retrieve(xref.Data());
         qr = fPlayer->GetQueryResult(xref.Data());
      }
      if (qr)
         return fPlayer->Finalize(qr);
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Retrieve(Int_t qry, const char *path)
{
   // Send retrieve request for the qry-th query in fQueries.
   // If path is defined save it to path.

   if (qry > 0) {
      TString ref;
      if (GetQueryReference(qry, ref) == 0)
         return Retrieve(ref, path);
      else
         Info("Retrieve", "query #%d not found", qry);
   } else {
      Info("Retrieve","positive argument required - do nothing");
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Retrieve(const char *ref, const char *path)
{
   // Send retrieve request for the query specified by ref.
   // If path is defined save it to path.
   // Generic method working for all queries known by the server.

   if (ref) {
      TMessage m(kPROOF_RETRIEVE);
      m << TString(ref);
      Broadcast(m, kActive);
      Collect(kActive, fCollectTimeout);

      // Archive it locally, if required
      if (path) {

         // Get pointer to query
         TQueryResult *qr = fPlayer ? fPlayer->GetQueryResult(ref) : 0;

         if (qr) {

            TFile *farc = TFile::Open(path,"UPDATE");
            if (!(farc->IsOpen())) {
               Info("Retrieve", "archive file cannot be open (%s)", path);
               return 0;
            }
            farc->cd();

            // Update query status
            qr->SetArchived(path);

            // Write to file
            qr->Write();

            farc->Close();
            SafeDelete(farc);

         } else {
            Info("Retrieve", "query not found after retrieve");
            return -1;
         }
      }

      return 0;
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Remove(Int_t qry, Bool_t all)
{
   // Send remove request for the qry-th query in fQueries.

   if (qry > 0) {
      TString ref;
      if (GetQueryReference(qry, ref) == 0)
         return Remove(ref, all);
      else
         Info("Remove", "query #%d not found", qry);
   } else {
      Info("Remove","positive argument required - do nothing");
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Remove(const char *ref, Bool_t all)
{
   // Send remove request for the query specified by ref.
   // If all = TRUE remove also local copies of the query, if any.
   // Generic method working for all queries known by the server.
   // This method can be also used to reset the list of queries
   // waiting to be processed: for that purpose use ref == "cleanupqueue".

   if (all) {
      // Remove also local copies, if any
      if (fPlayer)
         fPlayer->RemoveQueryResult(ref);
   }

   if (IsLite()) return 0;

   if (ref) {
      TMessage m(kPROOF_REMOVE);
      m << TString(ref);
      Broadcast(m, kActive);
      Collect(kActive, fCollectTimeout);
      return 0;
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Archive(Int_t qry, const char *path)
{
   // Send archive request for the qry-th query in fQueries.

   if (qry > 0) {
      TString ref;
      if (GetQueryReference(qry, ref) == 0)
         return Archive(ref, path);
      else
         Info("Archive", "query #%d not found", qry);
   } else {
      Info("Archive","positive argument required - do nothing");
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::Archive(const char *ref, const char *path)
{
   // Send archive request for the query specified by ref.
   // Generic method working for all queries known by the server.
   // If ref == "Default", path is understood as a default path for
   // archiving.

   if (ref) {
      TMessage m(kPROOF_ARCHIVE);
      m << TString(ref) << TString(path);
      Broadcast(m, kActive);
      Collect(kActive, fCollectTimeout);
      return 0;
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::CleanupSession(const char *sessiontag)
{
   // Send cleanup request for the session specified by tag.

   if (sessiontag) {
      TMessage m(kPROOF_CLEANUPSESSION);
      m << TString(sessiontag);
      Broadcast(m, kActive);
      Collect(kActive, fCollectTimeout);
      return 0;
   }
   return -1;
}

//_____________________________________________________________________________
void TProof::SetQueryMode(EQueryMode mode)
{
   // Change query running mode to the one specified by 'mode'.

   fQueryMode = mode;

   if (gDebug > 0)
      Info("SetQueryMode","query mode is set to: %s", fQueryMode == kSync ?
           "Sync" : "Async");
}

//______________________________________________________________________________
TProof::EQueryMode TProof::GetQueryMode(Option_t *mode) const
{
   // Find out the query mode based on the current setting and 'mode'.

   EQueryMode qmode = fQueryMode;

   if (mode && (strlen(mode) > 0)) {
      TString m(mode);
      m.ToUpper();
      if (m.Contains("ASYN")) {
         qmode = kAsync;
      } else if (m.Contains("SYNC")) {
         qmode = kSync;
      }
   }

   if (gDebug > 0)
      Info("GetQueryMode","query mode is set to: %s", qmode == kSync ?
           "Sync" : "Async");

   return qmode;
}

//______________________________________________________________________________
Long64_t TProof::DrawSelect(TDSet *dset, const char *varexp,
                            const char *selection, Option_t *option,
                            Long64_t nentries, Long64_t first)
{
   // Execute the specified drawing action on a data set (TDSet).
   // Event- or Entry-lists should be set in the data set object using
   // TDSet::SetEntryList.
   // Returns -1 in case of error or number of selected events otherwise.

   if (!IsValid() || !fPlayer) return -1;

   // Make sure that asynchronous processing is not active
   if (!IsIdle()) {
      Info("DrawSelect","not idle, asynchronous Draw not supported");
      return -1;
   }
   TString opt(option);
   Int_t idx = opt.Index("ASYN", 0, TString::kIgnoreCase);
   if (idx != kNPOS)
      opt.Replace(idx,4,"");

   return fPlayer->DrawSelect(dset, varexp, selection, opt, nentries, first);
}

//______________________________________________________________________________
Long64_t TProof::DrawSelect(const char *dsetname, const char *varexp,
                            const char *selection, Option_t *option,
                            Long64_t nentries, Long64_t first, TObject *enl)
{
   // Execute the specified drawing action on a data set which is stored on the
   // master with name 'dsetname'.
   // The syntax for dsetname is name[#[dir/]objname], e.g.
   //   "mydset"       analysis of the first tree in the top dir of the dataset
   //                  named "mydset"
   //   "mydset#T"     analysis tree "T" in the top dir of the dataset
   //                  named "mydset"
   //   "mydset#adir/T" analysis tree "T" in the dir "adir" of the dataset
   //                  named "mydset"
   //   "mydset#adir/" analysis of the first tree in the dir "adir" of the
   //                  dataset named "mydset"
   // The last argument 'enl' specifies an entry- or event-list to be used as
   // event selection.
   // The return value is -1 in case of error and TSelector::GetStatus() in
   // in case of success.

   if (fProtocol < 13) {
      Info("Process", "processing 'by name' not supported by the server");
      return -1;
   }

   TString name(dsetname);
   TString obj;
   TString dir = "/";
   Int_t idxc = name.Index("#");
   if (idxc != kNPOS) {
      Int_t idxs = name.Index("/", 1, idxc, TString::kExact);
      if (idxs != kNPOS && idxc != kNPOS) {
         obj = name(idxs+1, name.Length());
         dir = name(idxc+1, name.Length());
         dir.Remove(dir.Index("/") + 1);
         name.Remove(idxc);
      } else if (idxc != kNPOS && idxs == kNPOS) {
         obj = name(idxc+1, name.Length());
         name.Remove(idxc);
      } else if (idxs != kNPOS && idxc == kNPOS) {
         Error("DrawSelect", "bad name syntax (%s): specification of additional"
                          " attributes needs a '#' after the dataset name", dsetname);
         return -1;
      }
   } else if (name.Index(":") != kNPOS && name.Index("://") == kNPOS) {
      // protection against using ':' instead of '#'
      Error("DrawSelect", "bad name syntax (%s): please use"
                       " a '#' after the dataset name", dsetname);
      return -1;
   }

   TDSet *dset = new TDSet(name, obj, dir);
   // Set entry-list, if required
   dset->SetEntryList(enl);
   Long64_t retval = DrawSelect(dset, varexp, selection, option, nentries, first);
   delete dset;
   return retval;
}

//______________________________________________________________________________
void TProof::StopProcess(Bool_t abort, Int_t timeout)
{
   // Send STOPPROCESS message to master and workers.

   PDB(kGlobal,2)
      Info("StopProcess","enter %d", abort);

   if (!IsValid())
      return;

   // Flag that we have been stopped
   ERunStatus rst = abort ? TProof::kAborted : TProof::kStopped;
   SetRunStatus(rst);

   if (fPlayer)
      fPlayer->StopProcess(abort, timeout);

   // Stop any blocking 'Collect' request; on masters we do this only if
   // aborting; when stopping, we still need to receive the results
   if (TestBit(TProof::kIsClient) || abort)
      InterruptCurrentMonitor();

   if (fSlaves->GetSize() == 0)
      return;

   // Notify the remote counterpart
   TSlave *sl;
   TIter   next(fSlaves);
   while ((sl = (TSlave *)next()))
      if (sl->IsValid())
         // Ask slave to progate the stop/abort request
         sl->StopProcess(abort, timeout);
}

//______________________________________________________________________________
void TProof::RecvLogFile(TSocket *s, Int_t size)
{
   // Receive the log file of the slave with socket s.

   const Int_t kMAXBUF = 16384;  //32768  //16384  //65536;
   char buf[kMAXBUF];

   // Append messages to active logging unit
   Int_t fdout = -1;
   if (!fLogToWindowOnly) {
      fdout = (fRedirLog) ? fileno(fLogFileW) : fileno(stdout);
      if (fdout < 0) {
         Warning("RecvLogFile", "file descriptor for outputs undefined (%d):"
                 " will not log msgs", fdout);
         return;
      }
      lseek(fdout, (off_t) 0, SEEK_END);
   }

   Int_t  left, rec, r;
   Long_t filesize = 0;

   while (filesize < size) {
      left = Int_t(size - filesize);
      if (left > kMAXBUF)
         left = kMAXBUF;
      rec = s->RecvRaw(&buf, left);
      filesize = (rec > 0) ? (filesize + rec) : filesize;
      if (!fLogToWindowOnly) {
         if (rec > 0) {

            char *p = buf;
            r = rec;
            while (r) {
               Int_t w;

               w = write(fdout, p, r);

               if (w < 0) {
                  SysError("RecvLogFile", "error writing to unit: %d", fdout);
                  break;
               }
               r -= w;
               p += w;
            }
         } else if (rec < 0) {
            Error("RecvLogFile", "error during receiving log file");
            break;
         }
      }
      if (rec > 0) {
         buf[rec] = 0;
         EmitVA("LogMessage(const char*,Bool_t)", 2, buf, kFALSE);
      }
   }

   // If idle restore logs to main session window
   if (fRedirLog && IsIdle() && !TestBit(TProof::kIsMaster))
      fRedirLog = kFALSE;
}

//______________________________________________________________________________
void TProof::NotifyLogMsg(const char *msg, const char *sfx)
{
   // Notify locally 'msg' to the appropriate units (file, stdout, window)
   // If defined, 'sfx' is added after 'msg' (typically a line-feed);

   // Must have somenthing to notify
   Int_t len = 0;
   if (!msg || (len = strlen(msg)) <= 0)
      return;

   // Get suffix length if any
   Int_t lsfx = (sfx) ? strlen(sfx) : 0;

   // Append messages to active logging unit
   Int_t fdout = -1;
   if (!fLogToWindowOnly) {
      fdout = (fRedirLog) ? fileno(fLogFileW) : fileno(stdout);
      if (fdout < 0) {
         Warning("NotifyLogMsg", "file descriptor for outputs undefined (%d):"
                 " will not notify msgs", fdout);
         return;
      }
      lseek(fdout, (off_t) 0, SEEK_END);
   }

   if (!fLogToWindowOnly) {
      // Write to output unit (stdout or a log file)
      if (len > 0) {
         char *p = (char *)msg;
         Int_t r = len;
         while (r) {
            Int_t w = write(fdout, p, r);
            if (w < 0) {
               SysError("NotifyLogMsg", "error writing to unit: %d", fdout);
               break;
            }
            r -= w;
            p += w;
         }
         // Add a suffix, if requested
         if (lsfx > 0)
            if (write(fdout, sfx, lsfx) != lsfx)
               SysError("NotifyLogMsg", "error writing to unit: %d", fdout);
      }
   }
   if (len > 0) {
      // Publish the message to the separate window (if the latter is missing
      // the message will just get lost)
      EmitVA("LogMessage(const char*,Bool_t)", 2, msg, kFALSE);
   }

   // If idle restore logs to main session window
   if (fRedirLog && IsIdle())
      fRedirLog = kFALSE;
}

//______________________________________________________________________________
void TProof::LogMessage(const char *msg, Bool_t all)
{
   // Log a message into the appropriate window by emitting a signal.

   PDB(kGlobal,1)
      Info("LogMessage","Enter ... %s, 'all: %s", msg ? msg : "",
           all ? "true" : "false");

   if (gROOT->IsBatch()) {
      PDB(kGlobal,1) Info("LogMessage","GUI not started - use TProof::ShowLog()");
      return;
   }

   if (msg)
      EmitVA("LogMessage(const char*,Bool_t)", 2, msg, all);

   // Re-position at the beginning of the file, if requested.
   // This is used by the dialog when it re-opens the log window to
   // provide all the session messages
   if (all)
      lseek(fileno(fLogFileR), (off_t) 0, SEEK_SET);

   const Int_t kMAXBUF = 32768;
   char buf[kMAXBUF];
   Int_t len;
   do {
      while ((len = read(fileno(fLogFileR), buf, kMAXBUF-1)) < 0 &&
             TSystem::GetErrno() == EINTR)
         TSystem::ResetErrno();

      if (len < 0) {
         Error("LogMessage", "error reading log file");
         break;
      }

      if (len > 0) {
         buf[len] = 0;
         EmitVA("LogMessage(const char*,Bool_t)", 2, buf, kFALSE);
      }

   } while (len > 0);
}

//______________________________________________________________________________
Int_t TProof::SendGroupView()
{
   // Send to all active slaves servers the current slave group size
   // and their unique id. Returns number of active slaves.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;
   if (TestBit(TProof::kIsClient)) return 0;
   if (!fSendGroupView) return 0;
   fSendGroupView = kFALSE;

   TIter   next(fActiveSlaves);
   TSlave *sl;

   int  bad = 0, cnt = 0, size = GetNumberOfActiveSlaves();
   char str[32];

   while ((sl = (TSlave *)next())) {
      sprintf(str, "%d %d", cnt, size);
      if (sl->GetSocket()->Send(str, kPROOF_GROUPVIEW) == -1) {
         MarkBad(sl, "could not send kPROOF_GROUPVIEW message");
         bad++;
      } else
         cnt++;
   }

   // Send the group view again in case there was a change in the
   // group size due to a bad slave

   if (bad) SendGroupView();

   return GetNumberOfActiveSlaves();
}

//______________________________________________________________________________
Int_t TProof::Exec(const char *cmd, Bool_t plusMaster)
{
   // Send command to be executed on the PROOF master and/or slaves.
   // If plusMaster is kTRUE then exeucte on slaves and master too.
   // Command can be any legal command line command. Commands like
   // ".x file.C" or ".L file.C" will cause the file file.C to be send
   // to the PROOF cluster. Returns -1 in case of error, >=0 in case of
   // succes.

   return Exec(cmd, kActive, plusMaster);
}

//______________________________________________________________________________
Int_t TProof::Exec(const char *cmd, ESlaves list, Bool_t plusMaster)
{
   // Send command to be executed on the PROOF master and/or slaves.
   // Command can be any legal command line command. Commands like
   // ".x file.C" or ".L file.C" will cause the file file.C to be send
   // to the PROOF cluster. Returns -1 in case of error, >=0 in case of
   // succes.

   if (!IsValid()) return -1;

   TString s = cmd;
   s = s.Strip(TString::kBoth);

   if (!s.Length()) return 0;

   // check for macro file and make sure the file is available on all slaves
   if (s.BeginsWith(".L") || s.BeginsWith(".x") || s.BeginsWith(".X")) {
      TString file = s(2, s.Length());
      TString acm, arg, io;
      TString filename = gSystem->SplitAclicMode(file, acm, arg, io);
      char *fn = gSystem->Which(TROOT::GetMacroPath(), filename, kReadPermission);
      if (fn) {
         if (GetNumberOfUniqueSlaves() > 0) {
            if (SendFile(fn, kAscii | kForward | kCpBin) < 0) {
               Error("Exec", "file %s could not be transfered", fn);
               delete [] fn;
               return -1;
            }
         } else {
            TString scmd = s(0,3) + fn;
            Int_t n = SendCommand(scmd, list);
            delete [] fn;
            return n;
         }
      } else {
         Error("Exec", "macro %s not found", file.Data());
         return -1;
      }
      delete [] fn;
   }

   if (plusMaster) {
      if (IsLite()) {
         gROOT->ProcessLine(cmd);
      } else {
         Int_t n = GetParallel();
         SetParallelSilent(0);
         Int_t res = SendCommand(cmd, list);
         SetParallelSilent(n);
         if (res < 0)
            return res;
      }
   }
   return SendCommand(cmd, list);
}

//______________________________________________________________________________
Int_t TProof::SendCommand(const char *cmd, ESlaves list)
{
   // Send command to be executed on the PROOF master and/or slaves.
   // Command can be any legal command line command, however commands
   // like ".x file.C" or ".L file.C" will not cause the file.C to be
   // transfered to the PROOF cluster. In that case use TProof::Exec().
   // Returns the status send by the remote server as part of the
   // kPROOF_LOGDONE message. Typically this is the return code of the
   // command on the remote side. Returns -1 in case of error.

   if (!IsValid()) return -1;

   Broadcast(cmd, kMESS_CINT, list);
   Collect(list);

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::SendCurrentState(ESlaves list)
{
   // Transfer the current state of the master to the active slave servers.
   // The current state includes: the current working directory, etc.
   // Returns the number of active slaves. Returns -1 in case of error.

   if (!IsValid()) return -1;

   // Go to the new directory, reset the interpreter environment and
   // tell slave to delete all objects from its new current directory.
   Broadcast(gDirectory->GetPath(), kPROOF_RESET, list);

   return GetParallel();
}

//______________________________________________________________________________
Int_t TProof::SendInitialState()
{
   // Transfer the initial (i.e. current) state of the master to all
   // slave servers. Currently the initial state includes: log level.
   // Returns the number of active slaves. Returns -1 in case of error.

   if (!IsValid()) return -1;

   SetLogLevel(fLogLevel, gProofDebugMask);

   return GetNumberOfActiveSlaves();
}

//______________________________________________________________________________
Bool_t TProof::CheckFile(const char *file, TSlave *slave, Long_t modtime, Int_t cpopt)
{
   // Check if a file needs to be send to the slave. Use the following
   // algorithm:
   //   - check if file appears in file map
   //     - if yes, get file's modtime and check against time in map,
   //       if modtime not same get md5 and compare against md5 in map,
   //       if not same return kTRUE.
   //     - if no, get file's md5 and modtime and store in file map, ask
   //       slave if file exists with specific md5, if yes return kFALSE,
   //       if no return kTRUE.
   // The options 'cpopt' define if to copy things from cache to sandbox and what.
   // To retrieve from the cache the binaries associated with the file TProof::kCpBin
   // must be set in cpopt; the default is copy everything.
   // Returns kTRUE in case file needs to be send, returns kFALSE in case
   // file is already on remote node.

   Bool_t sendto = kFALSE;

   // create slave based filename
   TString sn = slave->GetName();
   sn += ":";
   sn += slave->GetOrdinal();
   sn += ":";
   sn += gSystem->BaseName(file);

   // check if file is in map
   FileMap_t::const_iterator it;
   if ((it = fFileMap.find(sn)) != fFileMap.end()) {
      // file in map
      MD5Mod_t md = (*it).second;
      if (md.fModtime != modtime) {
         TMD5 *md5 = TMD5::FileChecksum(file);
         if (md5) {
            if ((*md5) != md.fMD5) {
               sendto       = kTRUE;
               md.fMD5      = *md5;
               md.fModtime  = modtime;
               fFileMap[sn] = md;
               // When on the master, the master and/or slaves may share
               // their file systems and cache. Therefore always make a
               // check for the file. If the file already exists with the
               // expected md5 the kPROOF_CHECKFILE command will cause the
               // file to be copied from cache to slave sandbox.
               if (TestBit(TProof::kIsMaster)) {
                  sendto = kFALSE;
                  TMessage mess(kPROOF_CHECKFILE);
                  mess << TString(gSystem->BaseName(file)) << md.fMD5 << cpopt;
                  slave->GetSocket()->Send(mess);

                  fCheckFileStatus = 0;
                  Collect(slave, fCollectTimeout, kPROOF_CHECKFILE);
                  sendto = (fCheckFileStatus == 0) ? kTRUE : kFALSE;
               }
            }
            delete md5;
         } else {
            Error("CheckFile", "could not calculate local MD5 check sum - dont send");
            return kFALSE;
         }
      }
   } else {
      // file not in map
      TMD5 *md5 = TMD5::FileChecksum(file);
      MD5Mod_t md;
      if (md5) {
         md.fMD5      = *md5;
         md.fModtime  = modtime;
         fFileMap[sn] = md;
         delete md5;
      } else {
         Error("CheckFile", "could not calculate local MD5 check sum - dont send");
         return kFALSE;
      }
      TMessage mess(kPROOF_CHECKFILE);
      mess << TString(gSystem->BaseName(file)) << md.fMD5 << cpopt;
      slave->GetSocket()->Send(mess);

      fCheckFileStatus = 0;
      Collect(slave, fCollectTimeout, kPROOF_CHECKFILE);
      sendto = (fCheckFileStatus == 0) ? kTRUE : kFALSE;
   }

   return sendto;
}

//______________________________________________________________________________
Int_t TProof::SendFile(const char *file, Int_t opt, const char *rfile, TSlave *wrk)
{
   // Send a file to master or slave servers. Returns number of slaves
   // the file was sent to, maybe 0 in case master and slaves have the same
   // file system image, -1 in case of error.
   // If defined, send to worker 'wrk' only.
   // If defined, the full path of the remote path will be rfile.
   // If rfile = "cache" the file is copied to the remote cache instead of the sandbox
   // (to copy to the cache on a different name use rfile = "cache:newname").
   // The mask 'opt' is an or of ESendFileOpt:
   //
   //       kAscii  (0x0)      if set true ascii file transfer is used
   //       kBinary (0x1)      if set true binary file transfer is used
   //       kForce  (0x2)      if not set an attempt is done to find out
   //                          whether the file really needs to be downloaded
   //                          (a valid copy may already exist in the cache
   //                          from a previous run); the bit is set by
   //                          UploadPackage, since the check is done elsewhere.
   //       kForward (0x4)     if set, ask server to forward the file to slave
   //                          or submaster (meaningless for slave servers).
   //       kCpBin   (0x8)     Retrieve from the cache the binaries associated
   //                          with the file
   //       kCp      (0x10)    Retrieve the files from the cache
   //

   if (!IsValid()) return -1;

   // Use the active slaves list ...
   TList *slaves = (rfile && !strcmp(rfile, "cache")) ? fUniqueSlaves : fActiveSlaves;
   // ... or the specified slave, if any
   if (wrk) {
      slaves = new TList();
      slaves->Add(wrk);
   }

   if (slaves->GetSize() == 0) return 0;

#ifndef R__WIN32
   Int_t fd = open(file, O_RDONLY);
#else
   Int_t fd = open(file, O_RDONLY | O_BINARY);
#endif
   if (fd < 0) {
      SysError("SendFile", "cannot open file %s", file);
      return -1;
   }

   // Get info about the file
   Long64_t size;
   Long_t id, flags, modtime;
   if (gSystem->GetPathInfo(file, &id, &size, &flags, &modtime) == 1) {
      Error("SendFile", "cannot stat file %s", file);
      return -1;
   }
   if (size == 0) {
      Error("SendFile", "empty file %s", file);
      return -1;
   }

   // Decode options
   Bool_t bin   = (opt & kBinary)  ? kTRUE : kFALSE;
   Bool_t force = (opt & kForce)   ? kTRUE : kFALSE;
   Bool_t fw    = (opt & kForward) ? kTRUE : kFALSE;

   // Copy options
   Int_t cpopt  = 0;
   if ((opt & kCp)) cpopt |= kCp;
   if ((opt & kCpBin)) cpopt |= (kCp | kCpBin);

   const Int_t kMAXBUF = 32768;  //16384  //65536;
   char buf[kMAXBUF];
   Int_t nsl = 0;

   TIter next(slaves);
   TSlave *sl;
   TString fnam(rfile);
   if (fnam == "cache") {
      fnam += Form(":%s", gSystem->BaseName(file));
   } else if (fnam.IsNull()) {
      fnam = gSystem->BaseName(file);
   }
   // List on which we will collect the results
   TList wsent;
   while ((sl = (TSlave *)next())) {
      if (!sl->IsValid())
         continue;

      Bool_t sendto = force ? kTRUE : CheckFile(file, sl, modtime, cpopt);
      // Don't send the kPROOF_SENDFILE command to real slaves when sendto
      // is false. Masters might still need to send the file to newly added
      // slaves.
      PDB(kPackage,2) {
         const char *snd = (sl->fSlaveType == TSlave::kSlave && sendto) ? "" : "not";
         Info("SendFile", "%s sending file %s to: %s:%s (%d)", snd,
                    file, sl->GetName(), sl->GetOrdinal(), sendto);
      }
      if (sl->fSlaveType == TSlave::kSlave && !sendto)
         continue;
      // The value of 'size' is used as flag remotely, so we need to
      // reset it to 0 if we are not going to send the file
      Long64_t siz = sendto ? size : 0;
      sprintf(buf, "%s %d %lld %d", fnam.Data(), bin, siz, fw);
      if (sl->GetSocket()->Send(buf, kPROOF_SENDFILE) == -1) {
         MarkBad(sl, "could not send kPROOF_SENDFILE request");
         continue;
      }
      // Record
      wsent.Add(sl);

      if (sendto) {

         lseek(fd, 0, SEEK_SET);

         Int_t len;
         do {
            while ((len = read(fd, buf, kMAXBUF)) < 0 && TSystem::GetErrno() == EINTR)
               TSystem::ResetErrno();

            if (len < 0) {
               SysError("SendFile", "error reading from file %s", file);
               Interrupt(kSoftInterrupt, kActive);
               close(fd);
               return -1;
            }

            if (len > 0 && sl->GetSocket()->SendRaw(buf, len) == -1) {
               SysError("SendFile", "error writing to slave %s:%s (now offline)",
                        sl->GetName(), sl->GetOrdinal());
               MarkBad(sl, "sendraw failure");
               break;
            }

         } while (len > 0);

         nsl++;
      }
      // Wait for the operation to be done
      Collect(sl, fCollectTimeout, kPROOF_SENDFILE);
   }

   close(fd);

   // Cleanup temporary list, if any
   if (slaves != fActiveSlaves && slaves != fUniqueSlaves)
      SafeDelete(slaves);

   return nsl;
}

//______________________________________________________________________________
Int_t TProof::SendObject(const TObject *obj, ESlaves list)
{
   // Send object to master or slave servers. Returns number of slaves object
   // was sent to, -1 in case of error.

   if (!IsValid() || !obj) return -1;

   TMessage mess(kMESS_OBJECT);

   mess.WriteObject(obj);
   return Broadcast(mess, list);
}

//______________________________________________________________________________
Int_t TProof::SendPrint(Option_t *option)
{
   // Send print command to master server. Returns number of slaves message
   // was sent to. Returns -1 in case of error.

   if (!IsValid()) return -1;

   Broadcast(option, kPROOF_PRINT, kActive);
   return Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
void TProof::SetLogLevel(Int_t level, UInt_t mask)
{
   // Set server logging level.

   char str[32];
   fLogLevel        = level;
   gProofDebugLevel = level;
   gProofDebugMask  = (TProofDebug::EProofDebugMask) mask;
   sprintf(str, "%d %u", level, mask);
   Broadcast(str, kPROOF_LOGLEVEL, kAll);
}

//______________________________________________________________________________
void TProof::SetRealTimeLog(Bool_t on)
{
   // Switch ON/OFF the real-time logging facility. When this option is
   // ON, log messages from processing are sent back as they come, instead of
   // being sent back at the end in one go. This may help debugging or monitoring
   // in some cases, but, depending on the amount of log, it may have significant
   // consequencies on the load over the network, so it must be used with care.

   if (IsValid()) {
      TMessage mess(kPROOF_REALTIMELOG);
      mess << on;
      Broadcast(mess);
   } else {
      Warning("SetRealTimeLog","session is invalid - do nothing");
   }
}

//______________________________________________________________________________
Int_t TProof::SetParallelSilent(Int_t nodes, Bool_t random)
{
   // Tell PROOF how many slaves to use in parallel. If random is TRUE a random
   // selection is done (if nodes is less than the available nodes).
   // Returns the number of parallel slaves. Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (TestBit(TProof::kIsMaster)) {
      GoParallel(nodes, kFALSE, random);
      return SendCurrentState();
   } else {
      PDB(kGlobal,1) Info("SetParallelSilent", "request %d node%s", nodes,
          nodes == 1 ? "" : "s");
      TMessage mess(kPROOF_PARALLEL);
      mess << nodes << random;
      Broadcast(mess);
      Collect(kActive, fCollectTimeout);
      Int_t n = GetParallel();
      PDB(kGlobal,1) Info("SetParallelSilent", "got %d node%s", n, n == 1 ? "" : "s");
      return n;
   }
}

//______________________________________________________________________________
Int_t TProof::SetParallel(Int_t nodes, Bool_t random)
{
   // Tell PROOF how many slaves to use in parallel. Returns the number of
   // parallel slaves. Returns -1 in case of error.

   Int_t n = SetParallelSilent(nodes, random);
   if (TestBit(TProof::kIsClient)) {
      if (n < 1) {
         Printf("PROOF set to sequential mode");
      } else {
         TString subfix = (n == 1) ? "" : "s";
         if (random)
            subfix += ", randomly selected";
         Printf("PROOF set to parallel mode (%d worker%s)", n, subfix.Data());
      }
   }
   return n;
}

//______________________________________________________________________________
Int_t TProof::GoParallel(Int_t nodes, Bool_t attach, Bool_t random)
{
   // Go in parallel mode with at most "nodes" slaves. Since the fSlaves
   // list is sorted by slave performace the active list will contain first
   // the most performant nodes. Returns the number of active slaves.
   // If random is TRUE, and nodes is less than the number of available workers,
   // a random selection is done.
   // Returns -1 in case of error.

   if (!IsValid()) return -1;

   if (nodes < 0) nodes = 0;

   fActiveSlaves->Clear();
   fActiveMonitor->RemoveAll();

   // Prepare the list of candidates first.
   // Algorithm depends on random option.
   TSlave *sl = 0;
   TList *wlst = new TList;
   TIter nxt(fSlaves);
   fInactiveSlaves->Clear();
   while ((sl = (TSlave *)nxt())) {
      if (sl->IsValid() && !fBadSlaves->FindObject(sl)) {
         if (strcmp("IGNORE", sl->GetImage()) == 0) continue;
         if ((sl->GetSlaveType() != TSlave::kSlave) &&
             (sl->GetSlaveType() != TSlave::kMaster)) {
            Error("GoParallel", "TSlave is neither Master nor Slave");
            R__ASSERT(0);
         }
         // Good candidate
         wlst->Add(sl);
         // Set it inactive
         fInactiveSlaves->Add(sl);
         sl->SetStatus(TSlave::kInactive);
      }
   }
   Int_t nwrks = (nodes > wlst->GetSize()) ? wlst->GetSize() : nodes;
   int cnt = 0;
   fEndMaster = TestBit(TProof::kIsMaster) ? kTRUE : kFALSE;
   while (cnt < nwrks) {
      // Random choice, if requested
      if (random) {
         Int_t iwrk = (Int_t) (gRandom->Rndm() * wlst->GetSize());
         sl = (TSlave *) wlst->At(iwrk);
      } else {
         // The first available
         sl = (TSlave *) wlst->First();
      }
      if (!sl) {
         Error("GoParallel", "attaching to candidate!");
         break;
      }
      Int_t slavenodes = 0;
      if (sl->GetSlaveType() == TSlave::kSlave) {
         sl->SetStatus(TSlave::kActive);
         fActiveSlaves->Add(sl);
         fInactiveSlaves->Remove(sl);
         fActiveMonitor->Add(sl->GetSocket());
         slavenodes = 1;
      } else if (sl->GetSlaveType() == TSlave::kMaster) {
         fEndMaster = kFALSE;
         TMessage mess(kPROOF_PARALLEL);
         if (!attach) {
            mess << nodes-cnt;
         } else {
            // To get the number of slaves
            mess.SetWhat(kPROOF_LOGFILE);
            mess << -1 << -1;
         }
         if (sl->GetSocket()->Send(mess) == -1) {
            MarkBad(sl, "could not send kPROOF_PARALLEL or kPROOF_LOGFILE request");
            slavenodes = 0;
         } else {
            Collect(sl, fCollectTimeout);
            if (sl->IsValid()) {
               sl->SetStatus(TSlave::kActive);
               fActiveSlaves->Add(sl);
               fInactiveSlaves->Remove(sl);
               fActiveMonitor->Add(sl->GetSocket());
               if (sl->GetParallel() > 0) {
                  slavenodes = sl->GetParallel();
               } else {
                  slavenodes = 0;
               }
            } else {
               MarkBad(sl, "collect failed after kPROOF_PARALLEL or kPROOF_LOGFILE request");
               slavenodes = 0;
            }
         }
      }
      // Remove from the list
      wlst->Remove(sl);
//      cnt += slavenodes;
      cnt += 1;
   }

   // Cleanup list
   wlst->SetOwner(0);
   SafeDelete(wlst);

   // Get slave status (will set the slaves fWorkDir correctly)
   AskStatistics();

   // Find active slaves with unique image
   FindUniqueSlaves();

   // Send new group-view to slaves
   if (!attach)
      SendGroupView();

   Int_t n = GetParallel();

   if (TestBit(TProof::kIsClient)) {
      if (n < 1)
         printf("PROOF set to sequential mode\n");
      else
         printf("PROOF set to parallel mode (%d worker%s)\n",
                n, n == 1 ? "" : "s");
   }

   PDB(kGlobal,1) Info("GoParallel", "got %d node%s", n, n == 1 ? "" : "s");
   return n;
}

//______________________________________________________________________________
void TProof::ShowCache(Bool_t all)
{
   // List contents of file cache. If all is true show all caches also on
   // slaves. If everything is ok all caches are to be the same.

   if (!IsValid()) return;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kShowCache) << all;
   Broadcast(mess, kUnique);

   if (all) {
      TMessage mess2(kPROOF_CACHE);
      mess2 << Int_t(kShowSubCache) << all;
      Broadcast(mess2, fNonUniqueMasters);

      Collect(kAllUnique, fCollectTimeout);
   } else {
      Collect(kUnique, fCollectTimeout);
   }
}

//______________________________________________________________________________
void TProof::ClearCache(const char *file)
{
   // Remove file from all file caches. If file is 0 or "" or "*", remove all
   // the files

   if (!IsValid()) return;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kClearCache) << TString(file);
   Broadcast(mess, kUnique);

   TMessage mess2(kPROOF_CACHE);
   mess2 << Int_t(kClearSubCache) << TString(file);
   Broadcast(mess2, fNonUniqueMasters);

   Collect(kAllUnique);

   // clear file map so files get send again to remote nodes
   fFileMap.clear();
}

//______________________________________________________________________________
void TProof::ShowPackages(Bool_t all)
{
   // List contents of package directory. If all is true show all package
   // directries also on slaves. If everything is ok all package directories
   // should be the same.

   if (!IsValid()) return;

   if (TestBit(TProof::kIsClient)) {
      if (fGlobalPackageDirList && fGlobalPackageDirList->GetSize() > 0) {
         // Scan the list of global packages dirs
         TIter nxd(fGlobalPackageDirList);
         TNamed *nm = 0;
         while ((nm = (TNamed *)nxd())) {
            printf("*** Global Package cache %s client:%s ***\n",
                   nm->GetName(), nm->GetTitle());
            fflush(stdout);
            gSystem->Exec(Form("%s %s", kLS, nm->GetTitle()));
            printf("\n");
            fflush(stdout);
         }
      }
      printf("*** Package cache client:%s ***\n", fPackageDir.Data());
      fflush(stdout);
      gSystem->Exec(Form("%s %s", kLS, fPackageDir.Data()));
   }

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kShowPackages) << all;
   Broadcast(mess, kUnique);

   if (all) {
      TMessage mess2(kPROOF_CACHE);
      mess2 << Int_t(kShowSubPackages) << all;
      Broadcast(mess2, fNonUniqueMasters);

      Collect(kAllUnique, fCollectTimeout);
   } else {
      Collect(kUnique, fCollectTimeout);
   }
}

//______________________________________________________________________________
void TProof::ShowEnabledPackages(Bool_t all)
{
   // List which packages are enabled. If all is true show enabled packages
   // for all active slaves. If everything is ok all active slaves should
   // have the same packages enabled.

   if (!IsValid()) return;

   if (TestBit(TProof::kIsClient)) {
      printf("*** Enabled packages on client on %s\n", gSystem->HostName());
      TIter next(fEnabledPackagesOnClient);
      while (TObjString *str = (TObjString*) next())
         printf("%s\n", str->GetName());
   }

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kShowEnabledPackages) << all;
   Broadcast(mess);
   Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
Int_t TProof::ClearPackages()
{
   // Remove all packages.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (UnloadPackages() == -1)
      return -1;

   if (DisablePackages() == -1)
      return -1;

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::ClearPackage(const char *package)
{
   // Remove a specific package.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("ClearPackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   if (UnloadPackage(pac) == -1)
      return -1;

   if (DisablePackage(pac) == -1)
      return -1;

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::DisablePackage(const char *package)
{
   // Remove a specific package.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("DisablePackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   if (DisablePackageOnClient(pac) == -1)
      return -1;

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return 0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kDisablePackage) << pac;
   Broadcast(mess, kUnique);

   TMessage mess2(kPROOF_CACHE);
   mess2 << Int_t(kDisableSubPackage) << pac;
   Broadcast(mess2, fNonUniqueMasters);

   Collect(kAllUnique);

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::DisablePackageOnClient(const char *package)
{
   // Remove a specific package from the client.
   // Returns 0 in case of success and -1 in case of error.

   if (TestBit(TProof::kIsClient)) {
      // remove the package directory and the par file
      fPackageLock->Lock();
      gSystem->Exec(Form("%s %s/%s", kRM, fPackageDir.Data(), package));
      gSystem->Exec(Form("%s %s/%s.par", kRM, fPackageDir.Data(), package));
      fPackageLock->Unlock();
      if (!gSystem->AccessPathName(Form("%s/%s.par", fPackageDir.Data(), package)))
         Warning("DisablePackageOnClient", "unable to remove package PAR file for %s", package);
      if (!gSystem->AccessPathName(Form("%s/%s", fPackageDir.Data(), package)))
         Warning("DisablePackageOnClient", "unable to remove package directory for %s", package);
   }

   return 0;
}

//______________________________________________________________________________
Int_t TProof::DisablePackages()
{
   // Remove all packages.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   // remove all packages on client
   if (TestBit(TProof::kIsClient)) {
      fPackageLock->Lock();
      gSystem->Exec(Form("%s %s/*", kRM, fPackageDir.Data()));
      fPackageLock->Unlock();
   }

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return 0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kDisablePackages);
   Broadcast(mess, kUnique);

   TMessage mess2(kPROOF_CACHE);
   mess2 << Int_t(kDisableSubPackages);
   Broadcast(mess2, fNonUniqueMasters);

   Collect(kAllUnique);

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::BuildPackage(const char *package, EBuildPackageOpt opt)
{
   // Build specified package. Executes the PROOF-INF/BUILD.sh
   // script if it exists on all unique nodes. If opt is kBuildOnSlavesNoWait
   // then submit build command to slaves, but don't wait
   // for results. If opt is kCollectBuildResults then collect result
   // from slaves. To be used on the master.
   // If opt = kBuildAll (default) then submit and wait for results
   // (to be used on the client).
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("BuildPackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   Bool_t buildOnClient = kTRUE;
   if (opt == kDontBuildOnClient) {
      buildOnClient = kFALSE;
      opt = kBuildAll;
   }

   if (opt <= kBuildAll && !IsLite()) {
      TMessage mess(kPROOF_CACHE);
      mess << Int_t(kBuildPackage) << pac;
      Broadcast(mess, kUnique);

      TMessage mess2(kPROOF_CACHE);
      mess2 << Int_t(kBuildSubPackage) << pac;
      Broadcast(mess2, fNonUniqueMasters);
   }

   if (opt >= kBuildAll) {
      // by first forwarding the build commands to the master and slaves
      // and only then building locally we build in parallel
      Int_t st = 0;
      if (buildOnClient)
         st = BuildPackageOnClient(pac);

      fStatus = 0;
      if (!IsLite())
         Collect(kAllUnique);

      if (fStatus < 0 || st < 0)
         return -1;
   }

   return 0;
}

//______________________________________________________________________________
Int_t TProof::BuildPackageOnClient(const TString &package)
{
   // Build specified package on the client. Executes the PROOF-INF/BUILD.sh
   // script if it exists on the client.
   // Returns 0 in case of success and -1 in case of error.
   // The code is equivalent to the one in TProofServ.cxx (TProof::kBuildPackage
   // case). Keep in sync in case of changes.

   if (TestBit(TProof::kIsClient)) {
      Int_t status = 0;
      TString pdir, ocwd;

      // Package path
      pdir = fPackageDir + "/" + package;
      if (gSystem->AccessPathName(pdir, kReadPermission) ||
         gSystem->AccessPathName(pdir + "/PROOF-INF", kReadPermission)) {
         // Is there a global package with this name?
         if (fGlobalPackageDirList && fGlobalPackageDirList->GetSize() > 0) {
            // Scan the list of global packages dirs
            TIter nxd(fGlobalPackageDirList);
            TNamed *nm = 0;
            while ((nm = (TNamed *)nxd())) {
               pdir = Form("%s/%s", nm->GetTitle(), package.Data());
               if (!gSystem->AccessPathName(pdir, kReadPermission) &&
                   !gSystem->AccessPathName(pdir + "/PROOF-INF", kReadPermission)) {
                  // Package found, stop searching
                  break;
               }
               pdir = "";
            }
            if (pdir.Length() <= 0) {
               // Package not found
               Error("BuildPackageOnClient", "failure locating %s ...", package.Data());
               return -1;
            } else {
               // Package is in the global dirs
               if (gDebug > 0)
                  Info("BuildPackageOnClient", "found global package: %s", pdir.Data());
               return 0;
            }
         }
      }
      PDB(kPackage, 1)
         Info("BuildPackageOnCLient",
              "package %s exists and has PROOF-INF directory", package.Data());

      fPackageLock->Lock();

      ocwd = gSystem->WorkingDirectory();
      gSystem->ChangeDirectory(pdir);

      // check for BUILD.sh and execute
      if (!gSystem->AccessPathName("PROOF-INF/BUILD.sh")) {

         // read version from file proofvers.txt, and if current version is
         // not the same do a "BUILD.sh clean"
         Bool_t savever = kFALSE;
         Int_t rev = -1;
         TString v;
         FILE *f = fopen("PROOF-INF/proofvers.txt", "r");
         if (f) {
            TString r;
            v.Gets(f);
            r.Gets(f);
            rev = (!r.IsNull() && r.IsDigit()) ? r.Atoi() : -1;
            fclose(f);
         }
         if (!f || v != gROOT->GetVersion() ||
            (gROOT->GetSvnRevision() > 0 && rev != gROOT->GetSvnRevision())) {
            savever = kTRUE;
            Info("BuildPackageOnCLient",
                 "%s: version change (current: %s:%d, build: %s:%d): cleaning ... ",
                 package.Data(), gROOT->GetVersion(), gROOT->GetSvnRevision(), v.Data(), rev);
            // Hard cleanup: go up the dir tree
            gSystem->ChangeDirectory(fPackageDir);
            // remove package directory
            gSystem->Exec(Form("%s %s", kRM, pdir.Data()));
            // find gunzip...
            char *gunzip = gSystem->Which(gSystem->Getenv("PATH"), kGUNZIP, kExecutePermission);
            if (gunzip) {
               TString par = Form("%s.par", pdir.Data());
               // untar package
               TString cmd(Form(kUNTAR3, gunzip, par.Data()));
               status = gSystem->Exec(cmd);
               if ((status = gSystem->Exec(cmd))) {
                  Error("BuildPackageOnCLient", "failure executing: %s", cmd.Data());
               } else {
                  // Go down to the package directory
                  gSystem->ChangeDirectory(pdir);
               }
               delete [] gunzip;
            } else {
               Error("BuildPackageOnCLient", "%s not found", kGUNZIP);
               status = -1;
            }
         }

         if (gSystem->Exec("PROOF-INF/BUILD.sh")) {
            Error("BuildPackageOnClient", "building package %s on the client failed", package.Data());
            status = -1;
         }

         if (savever && !status) {
            f = fopen("PROOF-INF/proofvers.txt", "w");
            if (f) {
               fputs(gROOT->GetVersion(), f);
               fputs(Form("\n%d",gROOT->GetSvnRevision()), f);
               fclose(f);
            }
         }
      } else {
         PDB(kPackage, 1)
            Info("BuildPackageOnCLient",
                 "package %s exists but has no PROOF-INF/BUILD.sh script", package.Data());
      }

      gSystem->ChangeDirectory(ocwd);

      fPackageLock->Unlock();

      return status;
   }
   return 0;
}

//______________________________________________________________________________
Int_t TProof::LoadPackage(const char *package, Bool_t notOnClient)
{
   // Load specified package. Executes the PROOF-INF/SETUP.C script
   // on all active nodes. If notOnClient = true, don't load package
   // on the client. The default is to load the package also on the client.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("LoadPackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   if (!notOnClient)
      if (LoadPackageOnClient(pac) == -1)
         return -1;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kLoadPackage) << pac;
   Broadcast(mess);
   Collect();

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::LoadPackageOnClient(const TString &package)
{
   // Load specified package in the client. Executes the PROOF-INF/SETUP.C
   // script on the client. Returns 0 in case of success and -1 in case of error.
   // The code is equivalent to the one in TProofServ.cxx (TProof::kLoadPackage
   // case). Keep in sync in case of changes.

   if (TestBit(TProof::kIsClient)) {
      Int_t status = 0;
      TString pdir, ocwd;
      // If already loaded don't do it again
      if (fEnabledPackagesOnClient->FindObject(package)) {
         Info("LoadPackageOnClient",
              "package %s already loaded", package.Data());
         return 0;
      }

      // always follows BuildPackage so no need to check for PROOF-INF
      pdir = fPackageDir + "/" + package;

      if (gSystem->AccessPathName(pdir, kReadPermission)) {
         // Is there a global package with this name?
         if (fGlobalPackageDirList && fGlobalPackageDirList->GetSize() > 0) {
            // Scan the list of global packages dirs
            TIter nxd(fGlobalPackageDirList);
            TNamed *nm = 0;
            while ((nm = (TNamed *)nxd())) {
               pdir = Form("%s/%s", nm->GetTitle(), package.Data());
               if (!gSystem->AccessPathName(pdir, kReadPermission)) {
                  // Package found, stop searching
                  break;
               }
               pdir = "";
            }
            if (pdir.Length() <= 0) {
               // Package not found
               Error("LoadPackageOnClient", "failure locating %s ...", package.Data());
               return -1;
            }
         }
      }

      ocwd = gSystem->WorkingDirectory();
      gSystem->ChangeDirectory(pdir);

      // check for SETUP.C and execute
      if (!gSystem->AccessPathName("PROOF-INF/SETUP.C")) {
         Int_t err = 0;
         Int_t errm = gROOT->Macro("PROOF-INF/SETUP.C", &err);
         if (errm < 0)
            status = -1;
         if (err > TInterpreter::kNoError && err <= TInterpreter::kFatal)
            status = -1;
      } else {
         PDB(kPackage, 1)
            Info("LoadPackageOnCLient",
                 "package %s exists but has no PROOF-INF/SETUP.C script", package.Data());
      }

      gSystem->ChangeDirectory(ocwd);

      if (!status) {
         // create link to package in working directory

         fPackageLock->Lock();

         FileStat_t stat;
         Int_t st = gSystem->GetPathInfo(package, stat);
         // check if symlink, if so unlink, if not give error
         // NOTE: GetPathnfo() returns 1 in case of symlink that does not point to
         // existing file or to a directory, but if fIsLink is true the symlink exists
         if (stat.fIsLink)
            gSystem->Unlink(package);
         else if (st == 0) {
            Error("LoadPackageOnClient", "cannot create symlink %s in %s on client, "
                  "another item with same name already exists", package.Data(), ocwd.Data());
            fPackageLock->Unlock();
            return -1;
         }
         gSystem->Symlink(pdir, package);

         fPackageLock->Unlock();

         // add package to list of include directories to be searched by ACliC
         gSystem->AddIncludePath(TString("-I") + package);

         // add package to list of include directories to be searched by CINT
         gROOT->ProcessLine(TString(".include ") + package);

         fEnabledPackagesOnClient->Add(new TObjString(package));
         PDB(kPackage, 1)
            Info("LoadPackageOnClient",
                 "package %s successfully loaded", package.Data());
      } else
         Error("LoadPackageOnClient", "loading package %s on client failed", package.Data());

      return status;
   }
   return 0;
}

//______________________________________________________________________________
Int_t TProof::UnloadPackage(const char *package)
{
   // Unload specified package.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("UnloadPackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   if (UnloadPackageOnClient(pac) == -1)
      return -1;

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return 0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kUnloadPackage) << pac;
   Broadcast(mess);
   Collect();

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::UnloadPackageOnClient(const char *package)
{
   // Unload a specific package on the client.
   // Returns 0 in case of success and -1 in case of error.
   // The code is equivalent to the one in TProofServ.cxx (TProof::UnloadPackage
   // case). Keep in sync in case of changes.

   if (TestBit(TProof::kIsClient)) {
      TObjString *pack = (TObjString *) fEnabledPackagesOnClient->FindObject(package);
      if (pack) {
         // Remove entry from include path
         TString aclicincpath = gSystem->GetIncludePath();
         TString cintincpath = gInterpreter->GetIncludePath();
         // remove interpreter part of gSystem->GetIncludePath()
         aclicincpath.Remove(aclicincpath.Length() - cintincpath.Length() - 1);
         // remove package's include path
         aclicincpath.ReplaceAll(TString(" -I") + package, "");
         gSystem->SetIncludePath(aclicincpath);

         //TODO reset interpreter include path

         // remove entry from enabled packages list
         fEnabledPackagesOnClient->Remove(pack);
      }

      // cleanup the link
      if (!gSystem->AccessPathName(package))
         if (gSystem->Unlink(package) != 0)
            Warning("UnloadPackageOnClient", "unable to remove symlink to %s", package);

      // delete entry
      delete pack;
   }
   return 0;
}

//______________________________________________________________________________
Int_t TProof::UnloadPackages()
{
   // Unload all packages.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (TestBit(TProof::kIsClient)) {
      // Iterate over packages on the client and remove each package
      TIter nextpackage(fEnabledPackagesOnClient);
      while (TObjString *objstr = dynamic_cast<TObjString*>(nextpackage()))
         if (UnloadPackageOnClient(objstr->String()) == -1 )
            return -1;
   }

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return 0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kUnloadPackages);
   Broadcast(mess);
   Collect();

   return fStatus;
}

//______________________________________________________________________________
Int_t TProof::EnablePackage(const char *package, Bool_t notOnClient)
{
   // Enable specified package. Executes the PROOF-INF/BUILD.sh
   // script if it exists followed by the PROOF-INF/SETUP.C script.
   // In case notOnClient = true, don't enable the package on the client.
   // The default is to enable packages also on the client.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (!package || !strlen(package)) {
      Error("EnablePackage", "need to specify a package name");
      return -1;
   }

   // if name, erroneously, is a par pathname strip off .par and path
   TString pac = package;
   if (pac.EndsWith(".par"))
      pac.Remove(pac.Length()-4);
   pac = gSystem->BaseName(pac);

   EBuildPackageOpt opt = kBuildAll;
   if (notOnClient)
      opt = kDontBuildOnClient;

   if (BuildPackage(pac, opt) == -1)
      return -1;

   if (LoadPackage(pac, notOnClient) == -1)
      return -1;

   return 0;
}

//______________________________________________________________________________
Int_t TProof::UploadPackage(const char *pack, EUploadPackageOpt opt)
{
   // Upload a PROOF archive (PAR file). A PAR file is a compressed
   // tar file with one special additional directory, PROOF-INF
   // (blatantly copied from Java's jar format). It must have the extension
   // .par. A PAR file can be directly a binary or a source with a build
   // procedure. In the PROOF-INF directory there can be a build script:
   // BUILD.sh to be called to build the package, in case of a binary PAR
   // file don't specify a build script or make it a no-op. Then there is
   // SETUP.C which sets the right environment variables to use the package,
   // like LD_LIBRARY_PATH, etc.
   // The 'opt' allows to specify whether the .PAR should be just unpacked
   // in the existing dir (opt = kUntar, default) or a remove of the existing
   // directory should be executed (opt = kRemoveOld), so triggering a full
   // re-build. The option if effective only for PROOF protocol > 8 .
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   TString par = pack;
   if (!par.EndsWith(".par"))
      // The client specified only the name: add the extension
      par += ".par";

   // Default location is the local working dir; then the package dir
   gSystem->ExpandPathName(par);
   if (gSystem->AccessPathName(par, kReadPermission)) {
      TString tried = par;
      // Try the package dir
      par = Form("%s/%s", fPackageDir.Data(), gSystem->BaseName(par));
      if (gSystem->AccessPathName(par, kReadPermission)) {
         // Is the package a global one
         if (fGlobalPackageDirList && fGlobalPackageDirList->GetSize() > 0) {
            // Scan the list of global packages dirs
            TIter nxd(fGlobalPackageDirList);
            TNamed *nm = 0;
            TString pdir;
            while ((nm = (TNamed *)nxd())) {
               pdir = Form("%s/%s", nm->GetTitle(), pack);
               if (!gSystem->AccessPathName(pdir, kReadPermission)) {
                  // Package found, stop searching
                  break;
               }
               pdir = "";
            }
            if (pdir.Length() > 0) {
               // Package is in the global dirs
               if (gDebug > 0)
                  Info("UploadPackage", "global package found (%s): no upload needed",
                                        pdir.Data());
               return 0;
            }
         }
         Error("UploadPackage", "PAR file '%s' not found; paths tried: %s, %s",
                                gSystem->BaseName(par), tried.Data(), par.Data());
         return -1;
      }
   }

   // Strategy:
   // On the client:
   // get md5 of package and check if it is different
   // from the one stored in the local package directory. If it is lock
   // the package directory and copy the package, unlock the directory.
   // On the masters:
   // get md5 of package and check if it is different from the
   // one stored on the remote node. If it is different lock the remote
   // package directory and use TFTP or SendFile to ftp the package to the
   // remote node, unlock the directory.

   TMD5 *md5 = TMD5::FileChecksum(par);

   if (UploadPackageOnClient(par, opt, md5) == -1) {
      delete md5;
      return -1;
   }

   // Nothing more to do if we are a Lite-session
   if (IsLite()) return 0;

   TString smsg;
   smsg.Form("+%s", gSystem->BaseName(par));

   TMessage mess(kPROOF_CHECKFILE);
   mess << smsg << (*md5);
   TMessage mess2(kPROOF_CHECKFILE);
   smsg.Replace(0, 1, "-");
   mess2 << smsg << (*md5);
   TMessage mess3(kPROOF_CHECKFILE);
   smsg.Replace(0, 1, "=");
   mess3 << smsg << (*md5);

   delete md5;

   if (fProtocol > 8) {
      // Send also the option
      mess << (UInt_t) opt;
      mess2 << (UInt_t) opt;
      mess3 << (UInt_t) opt;
   }

   // loop over all selected nodes
   TIter next(fUniqueSlaves);
   TSlave *sl = 0;
   while ((sl = (TSlave *) next())) {
      if (!sl->IsValid())
         continue;

      sl->GetSocket()->Send(mess);

      fCheckFileStatus = 0;
      Collect(sl, fCollectTimeout, kPROOF_CHECKFILE);
      if (fCheckFileStatus == 0) {

         if (fProtocol > 5) {
            // remote directory is locked, upload file over the open channel
            smsg.Form("%s/%s/%s", sl->GetProofWorkDir(), kPROOF_PackDir,
                                  gSystem->BaseName(par));
            if (SendFile(par, (kBinary | kForce | kCpBin | kForward), smsg.Data(), sl) < 0) {
               Error("UploadPackage", "%s: problems uploading file %s",
                                      sl->GetOrdinal(), par.Data());
               return -1;
            }
         } else {
            // old servers receive it via TFTP
            TFTP ftp(TString("root://")+sl->GetName(), 1);
            if (!ftp.IsZombie()) {
               smsg.Form("%s/%s", sl->GetProofWorkDir(), kPROOF_PackDir);
               ftp.cd(smsg.Data());
               ftp.put(par, gSystem->BaseName(par));
            }
         }

         // install package and unlock dir
         sl->GetSocket()->Send(mess2);
         fCheckFileStatus = 0;
         Collect(sl, fCollectTimeout, kPROOF_CHECKFILE);
         if (fCheckFileStatus == 0) {
            Error("UploadPackage", "%s: unpacking of package %s failed",
                                   sl->GetOrdinal(), gSystem->BaseName(par));
            return -1;
         }
      }
   }

   // loop over all other master nodes
   TIter nextmaster(fNonUniqueMasters);
   TSlave *ma;
   while ((ma = (TSlave *) nextmaster())) {
      if (!ma->IsValid())
         continue;

      ma->GetSocket()->Send(mess3);

      fCheckFileStatus = 0;
      Collect(sl, fCollectTimeout, kPROOF_CHECKFILE);
      if (fCheckFileStatus == 0) {
         // error -> package should have been found
         Error("UploadPackage", "package %s did not exist on submaster %s",
               par.Data(), ma->GetOrdinal());
         return -1;
      }
   }

   return 0;
}

//______________________________________________________________________________
Int_t TProof::UploadPackageOnClient(const TString &par, EUploadPackageOpt opt, TMD5 *md5)
{
   // Upload a package on the client in ~/proof/packages.
   // The 'opt' allows to specify whether the .PAR should be just unpacked
   // in the existing dir (opt = kUntar, default) or a remove of the existing
   // directory should be executed (opt = kRemoveOld), thereby triggering a full
   // re-build. The option if effective only for PROOF protocol > 8 .
   // Returns 0 in case of success and -1 in case of error.

   // Strategy:
   // get md5 of package and check if it is different
   // from the one stored in the local package directory. If it is lock
   // the package directory and copy the package, unlock the directory.

   Int_t status = 0;

   if (TestBit(TProof::kIsClient)) {
      // the fPackageDir directory exists (has been created in Init())

      // create symlink to the par file in the fPackageDir (needed by
      // master in case we run on the localhost)
      fPackageLock->Lock();

      TString lpar = fPackageDir + "/" + gSystem->BaseName(par);
      FileStat_t stat;
      Int_t st = gSystem->GetPathInfo(lpar, stat);
      // check if symlink, if so unlink, if not give error
      // NOTE: GetPathInfo() returns 1 in case of symlink that does not point to
      // existing file, but if fIsLink is true the symlink exists
      if (stat.fIsLink)
         gSystem->Unlink(lpar);
      else if (st == 0) {
         Error("UploadPackageOnClient", "cannot create symlink %s on client, "
               "another item with same name already exists",
               lpar.Data());
         fPackageLock->Unlock();
         return -1;
      }
      if (!gSystem->IsAbsoluteFileName(par)) {
         TString fpar = par;
         gSystem->Symlink(gSystem->PrependPathName(gSystem->WorkingDirectory(), fpar), lpar);
      } else
         gSystem->Symlink(par, lpar);
      // TODO: On Windows need to copy instead of symlink

      // compare md5
      TString packnam = par(0, par.Length() - 4);  // strip off ".par"
      packnam = gSystem->BaseName(packnam);        // strip off path
      TString md5f = fPackageDir + "/" + packnam + "/PROOF-INF/md5.txt";
      TMD5 *md5local = TMD5::ReadChecksum(md5f);
      if (!md5local || (*md5) != (*md5local)) {
         // if not, unzip and untar package in package directory
         if ((opt & TProof::kRemoveOld)) {
            // remove any previous package directory with same name
            if (gSystem->Exec(Form("%s %s/%s", kRM, fPackageDir.Data(),
                                   packnam.Data())))
               Error("UploadPackageOnClient", "failure executing: %s %s/%s",
                     kRM, fPackageDir.Data(), packnam.Data());
         }
         // find gunzip
         char *gunzip = gSystem->Which(gSystem->Getenv("PATH"), kGUNZIP,
                                       kExecutePermission);
         if (gunzip) {
            // untar package
            if (gSystem->Exec(Form(kUNTAR2, gunzip, par.Data(), fPackageDir.Data())))
               Error("Uploadpackage", "failure executing: %s",
                     Form(kUNTAR2, gunzip, par.Data(), fPackageDir.Data()));
            delete [] gunzip;
         } else
            Error("UploadPackageOnClient", "%s not found", kGUNZIP);

         // check that fPackageDir/packnam now exists
         if (gSystem->AccessPathName(fPackageDir + "/" + packnam, kWritePermission)) {
            // par file did not unpack itself in the expected directory, failure
            Error("UploadPackageOnClient",
                  "package %s did not unpack into %s/%s", par.Data(), fPackageDir.Data(),
                  packnam.Data());
            status = -1;
         } else {
            // store md5 in package/PROOF-INF/md5.txt
            TMD5::WriteChecksum(md5f, md5);
         }
      }
      fPackageLock->Unlock();
      delete md5local;
   }
   return status;
}

//______________________________________________________________________________
Int_t TProof::Load(const char *macro, Bool_t notOnClient, Bool_t uniqueWorkers,
                   TList *wrks)
{
   // Load the specified macro on master, workers and, if notOnClient is
   // kFALSE, on the client. The macro file is uploaded if new or updated.
   // If existing, the corresponding header basename(macro).h or .hh, is also
   // uploaded. The default is to load the macro also on the client.
   // On masters, if uniqueWorkers is kTRUE, the macro is loaded on unique workers
   // only, and collection is not done; if uniqueWorkers is kFALSE, collection
   // from the previous request is done, and broadcasting + collection from the
   // other workers is done.
   // The wrks arg can be used on the master to limit the set of workers.
   // Returns 0 in case of success and -1 in case of error.

   if (!IsValid()) return -1;

   if (IsLite()) {
      Warning("Load", "functionality not yet implemented; please use Exec(...)"
                      " or a dedicated PAR package");
      return -1;
   }

   if (!macro || !strlen(macro)) {
      Error("Load", "need to specify a macro name");
      return -1;
   }

   if (TestBit(TProof::kIsClient)) {
      if (wrks) {
         Error("Load", "the 'wrks' arg can be used only on the master");
         return -1;
      }

      // Extract the file implementation name first
      TString implname = macro;
      TString acmode, args, io;
      implname = gSystem->SplitAclicMode(implname, acmode, args, io);

      // Macro names must have a standard format
      Int_t dot = implname.Last('.');
      if (dot == kNPOS) {
         Info("Load", "macro '%s' does not contain a '.': do nothing", macro);
         return -1;
      }

      // Is there any associated header file
      Bool_t hasHeader = kTRUE;
      TString headname = implname;
      headname.Remove(dot);
      headname += ".h";
      if (gSystem->AccessPathName(headname, kReadPermission)) {
         TString h = headname;
         headname.Remove(dot);
         headname += ".hh";
         if (gSystem->AccessPathName(headname, kReadPermission)) {
            hasHeader = kFALSE;
            if (gDebug > 0)
               Info("Load", "no associated header file found: tried: %s %s",
                            h.Data(), headname.Data());
         }
      }

      // Send files now; the md5 check is run here; see SendFile for more
      // details.
      if (SendFile(implname, kAscii | kForward , "cache") == -1) {
         Info("Load", "problems sending implementation file %s", implname.Data());
         return -1;
      }
      if (hasHeader)
         if (SendFile(headname, kAscii | kForward , "cache") == -1) {
            Info("Load", "problems sending header file %s", headname.Data());
            return -1;
         }

      // The files are now on the workers: now we send the loading request
      TString basemacro = gSystem->BaseName(macro);
      TMessage mess(kPROOF_CACHE);
      mess << Int_t(kLoadMacro) << basemacro;
      Broadcast(mess, kActive);

      // Load locally, if required
      if (!notOnClient) {
         // by first forwarding the load command to the master and workers
         // and only then loading locally we load/build in parallel
         gROOT->ProcessLine(Form(".L %s", macro));

         // Update the macro path
         TString mp(TROOT::GetMacroPath());
         TString np(gSystem->DirName(macro));
         if (!np.IsNull()) {
            np += ":";
            Int_t ip = (mp.BeginsWith(".:")) ? 2 : 0;
            mp.Insert(ip, np);
         }
         TROOT::SetMacroPath(mp);
         if (gDebug > 0)
            Info("Load", "macro path set to '%s'", TROOT::GetMacroPath());
      }

      // Wait for master and workers to be done
      Collect(kActive);

   } else {
      // On master

      // The files are now on the workers: now we send the loading request first
      // to the unique workers, so that the eventual compilation occurs only once.
      TString basemacro = gSystem->BaseName(macro);
      TMessage mess(kPROOF_CACHE);

      if (uniqueWorkers) {
         mess << Int_t(kLoadMacro) << basemacro;
         if (wrks)
            Broadcast(mess, wrks);
         else
            Broadcast(mess, kUnique);
      } else {
         // Wait for the result of the previous sending
         Collect(kUnique);

         // We then send a tuned loading request to the other workers
         TList others;
         TSlave *wrk = 0;
         TIter nxw(fActiveSlaves);
         while ((wrk = (TSlave *)nxw())) {
            if (!fUniqueSlaves->FindObject(wrk)) {
               others.Add(wrk);
            }
         }

         // Do not force compilation, if it was requested
         Int_t ld = basemacro.Last('.');
         if (ld != kNPOS) {
            Int_t lpp = basemacro.Index("++", ld);
            if (lpp != kNPOS) basemacro.Replace(lpp, 2, "+");
         }
         mess << Int_t(kLoadMacro) << basemacro;
         Broadcast(mess, &others);
         Collect(&others);
      }

      PDB(kGlobal, 1) Info("Load", "adding loaded macro: %s", macro);
      if (!fLoadedMacros) {
         fLoadedMacros = new TList();
         fLoadedMacros->SetOwner();
      }
      // if wrks is specified the macro should already be loaded on the master.
      if (!wrks)
         fLoadedMacros->Add(new TObjString(macro));
   }

   // Done
   return 0;
}

//______________________________________________________________________________
Int_t TProof::AddDynamicPath(const char *libpath, Bool_t onClient, TList *wrks)
{
   // Add 'libpath' to the lib path search.
   // Multiple paths can be specified at once separating them with a comma or
   // a blank.
   // Return 0 on success, -1 otherwise

   if ((!libpath || !strlen(libpath))) {
      if (gDebug > 0)
         Info("AddDynamicPath", "list is empty - nothing to do");
      return 0;
   }

   // Do it also on clients, if required
   if (onClient)
      HandleLibIncPath("lib", kTRUE, libpath);

   TMessage m(kPROOF_LIB_INC_PATH);
   m << TString("lib") << (Bool_t)kTRUE;

   // Add paths
   if (libpath && strlen(libpath))
      m << TString(libpath);
   else
      m << TString("-");

   // Forward the request
   if (wrks)
      Broadcast(m, wrks);
   else
      Broadcast(m);
   Collect(kActive, fCollectTimeout);

   return 0;
}

//______________________________________________________________________________
Int_t TProof::AddIncludePath(const char *incpath, Bool_t onClient, TList *wrks)
{
   // Add 'incpath' to the inc path search.
   // Multiple paths can be specified at once separating them with a comma or
   // a blank.
   // Return 0 on success, -1 otherwise

   if ((!incpath || !strlen(incpath))) {
      if (gDebug > 0)
         Info("AddIncludePath", "list is empty - nothing to do");
      return 0;
   }

   // Do it also on clients, if required
   if (onClient)
      HandleLibIncPath("inc", kTRUE, incpath);

   TMessage m(kPROOF_LIB_INC_PATH);
   m << TString("inc") << (Bool_t)kTRUE;

   // Add paths
   if (incpath && strlen(incpath))
      m << TString(incpath);
   else
      m << TString("-");

   // Forward the request
   if (wrks)
      Broadcast(m, wrks);
   else
      Broadcast(m);
   Collect(kActive, fCollectTimeout);

   return 0;
}

//______________________________________________________________________________
Int_t TProof::RemoveDynamicPath(const char *libpath, Bool_t onClient)
{
   // Remove 'libpath' from the lib path search.
   // Multiple paths can be specified at once separating them with a comma or
   // a blank.
   // Return 0 on success, -1 otherwise

   if ((!libpath || !strlen(libpath))) {
      if (gDebug > 0)
         Info("RemoveDynamicPath", "list is empty - nothing to do");
      return 0;
   }

   // Do it also on clients, if required
   if (onClient)
      HandleLibIncPath("lib", kFALSE, libpath);

   TMessage m(kPROOF_LIB_INC_PATH);
   m << TString("lib") <<(Bool_t)kFALSE;

   // Add paths
   if (libpath && strlen(libpath))
      m << TString(libpath);
   else
      m << TString("-");

   // Forward the request
   Broadcast(m);
   Collect(kActive, fCollectTimeout);

   return 0;
}

//______________________________________________________________________________
Int_t TProof::RemoveIncludePath(const char *incpath, Bool_t onClient)
{
   // Remove 'incpath' from the inc path search.
   // Multiple paths can be specified at once separating them with a comma or
   // a blank.
   // Return 0 on success, -1 otherwise

   if ((!incpath || !strlen(incpath))) {
      if (gDebug > 0)
         Info("RemoveIncludePath", "list is empty - nothing to do");
      return 0;
   }

   // Do it also on clients, if required
   if (onClient)
      HandleLibIncPath("in", kFALSE, incpath);

   TMessage m(kPROOF_LIB_INC_PATH);
   m << TString("inc") << (Bool_t)kFALSE;

   // Add paths
   if (incpath && strlen(incpath))
      m << TString(incpath);
   else
      m << TString("-");

   // Forward the request
   Broadcast(m);
   Collect(kActive, fCollectTimeout);

   return 0;
}

//______________________________________________________________________________
void TProof::HandleLibIncPath(const char *what, Bool_t add, const char *dirs)
{
   // Handle lib, inc search paths modification request

   TString type(what);
   TString path(dirs);

   // Check type of action
   if ((type != "lib") && (type != "inc")) {
      Error("HandleLibIncPath","unknown action type: %s", type.Data());
      return;
   }

   // Separators can be either commas or blanks
   path.ReplaceAll(","," ");

   // Decompose lists
   TObjArray *op = 0;
   if (path.Length() > 0 && path != "-") {
      if (!(op = path.Tokenize(" "))) {
         Error("HandleLibIncPath","decomposing path %s", path.Data());
         return;
      }
   }

   if (add) {

      if (type == "lib") {

         // Add libs
         TIter nxl(op, kIterBackward);
         TObjString *lib = 0;
         while ((lib = (TObjString *) nxl())) {
            // Expand path
            TString xlib = lib->GetName();
            gSystem->ExpandPathName(xlib);
            // Add to the dynamic lib search path if it exists and can be read
            if (!gSystem->AccessPathName(xlib, kReadPermission)) {
               TString newlibpath = gSystem->GetDynamicPath();
               // In the first position after the working dir
               Int_t pos = 0;
               if (newlibpath.BeginsWith(".:"))
                  pos = 2;
               if (newlibpath.Index(xlib) == kNPOS) {
                  newlibpath.Insert(pos,Form("%s:", xlib.Data()));
                  gSystem->SetDynamicPath(newlibpath);
               }
            } else {
               Info("HandleLibIncPath",
                    "libpath %s does not exist or cannot be read - not added", xlib.Data());
            }
         }

      } else {

         // Add incs
         TIter nxi(op);
         TObjString *inc = 0;
         while ((inc = (TObjString *) nxi())) {
            // Expand path
            TString xinc = inc->GetName();
            gSystem->ExpandPathName(xinc);
            // Add to the dynamic lib search path if it exists and can be read
            if (!gSystem->AccessPathName(xinc, kReadPermission)) {
               TString curincpath = gSystem->GetIncludePath();
               if (curincpath.Index(xinc) == kNPOS)
                  gSystem->AddIncludePath(Form("-I%s", xinc.Data()));
            } else
               Info("HandleLibIncPath",
                    "incpath %s does not exist or cannot be read - not added", xinc.Data());
         }
      }


   } else {

      if (type == "lib") {

         // Remove libs
         TIter nxl(op);
         TObjString *lib = 0;
         while ((lib = (TObjString *) nxl())) {
            // Expand path
            TString xlib = lib->GetName();
            gSystem->ExpandPathName(xlib);
            // Remove from the dynamic lib search path
            TString newlibpath = gSystem->GetDynamicPath();
            newlibpath.ReplaceAll(Form("%s:", xlib.Data()),"");
            gSystem->SetDynamicPath(newlibpath);
         }

      } else {

         // Remove incs
         TIter nxi(op);
         TObjString *inc = 0;
         while ((inc = (TObjString *) nxi())) {
            TString newincpath = gSystem->GetIncludePath();
            newincpath.ReplaceAll(Form("-I%s", inc->GetName()),"");
            // Remove the interpreter path (added anyhow internally)
            newincpath.ReplaceAll(gInterpreter->GetIncludePath(),"");
            gSystem->SetIncludePath(newincpath);
         }
      }
   }
}

//______________________________________________________________________________
TList *TProof::GetListOfPackages()
{
   // Get from the master the list of names of the packages available.

   if (!IsValid())
      return (TList *)0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kListPackages);
   Broadcast(mess);
   Collect(kActive, fCollectTimeout);

   return fAvailablePackages;
}

//______________________________________________________________________________
TList *TProof::GetListOfEnabledPackages()
{
   // Get from the master the list of names of the packages enabled.

   if (!IsValid())
      return (TList *)0;

   TMessage mess(kPROOF_CACHE);
   mess << Int_t(kListEnabledPackages);
   Broadcast(mess);
   Collect(kActive, fCollectTimeout);

   return fEnabledPackages;
}

//______________________________________________________________________________
void TProof::PrintProgress(Long64_t total, Long64_t processed, Float_t procTime)
{
   // Print a progress bar on stderr. Used in batch mode.

   if (fPrintProgress) {
      Bool_t redirlog = fRedirLog;
      fRedirLog = kFALSE;
      // Call the external function
      (*fPrintProgress)(total, processed, procTime);
      fRedirLog = redirlog;
      return;
   }

   fprintf(stderr, "[TProof::Progress] Total %lld events\t|", total);

   for (int l = 0; l < 20; l++) {
      if (total > 0) {
         if (l < 20*processed/total)
            fprintf(stderr, "=");
         else if (l == 20*processed/total)
            fprintf(stderr, ">");
         else if (l > 20*processed/total)
            fprintf(stderr, ".");
      } else
         fprintf(stderr, "=");
   }
   Float_t evtrti = (procTime > 0. && processed > 0) ? processed / procTime : -1.;
   if (evtrti > 0.)
      fprintf(stderr, "| %.02f %% [%.1f evts/s]\r",
              (total ? ((100.0*processed)/total) : 100.0), evtrti);
   else
      fprintf(stderr, "| %.02f %%\r",
              (total ? ((100.0*processed)/total) : 100.0));
   if (processed >= total)
      fprintf(stderr, "\n");
}

//______________________________________________________________________________
void TProof::Progress(Long64_t total, Long64_t processed)
{
   // Get query progress information. Connect a slot to this signal
   // to track progress.

   if (fPrintProgress) {
      // Call the external function
      return (*fPrintProgress)(total, processed, -1.);
   }

   PDB(kGlobal,1)
      Info("Progress","%2f (%lld/%lld)", 100.*processed/total, processed, total);

   if (gROOT->IsBatch()) {
      // Simple progress bar
      if (total > 0)
         PrintProgress(total, processed);
   } else {
      EmitVA("Progress(Long64_t,Long64_t)", 2, total, processed);
   }
}

//______________________________________________________________________________
void TProof::Progress(Long64_t total, Long64_t processed, Long64_t bytesread,
                      Float_t initTime, Float_t procTime,
                      Float_t evtrti, Float_t mbrti)
{
   // Get query progress information. Connect a slot to this signal
   // to track progress.

   PDB(kGlobal,1)
      Info("Progress","%lld %lld %lld %f %f %f %f", total, processed, bytesread,
                                initTime, procTime, evtrti, mbrti);

   if (gROOT->IsBatch()) {
      // Simple progress bar
      if (total > 0)
         PrintProgress(total, processed, procTime);
   } else {
      EmitVA("Progress(Long64_t,Long64_t,Long64_t,Float_t,Float_t,Float_t,Float_t)",
             7, total, processed, bytesread, initTime, procTime, evtrti, mbrti);
   }
}

//______________________________________________________________________________
void TProof::Feedback(TList *objs)
{
   // Get list of feedback objects. Connect a slot to this signal
   // to monitor the feedback object.

   PDB(kGlobal,1)
      Info("Feedback","%d objects", objs->GetSize());
   PDB(kFeedback,1) {
      Info("Feedback","%d objects", objs->GetSize());
      objs->ls();
   }

   Emit("Feedback(TList *objs)", (Long_t) objs);
}

//______________________________________________________________________________
void TProof::CloseProgressDialog()
{
   // Close progress dialog.

   PDB(kGlobal,1)
      Info("CloseProgressDialog",
           "called: have progress dialog: %d", fProgressDialogStarted);

   // Nothing to do if not there
   if (!fProgressDialogStarted)
      return;

   Emit("CloseProgressDialog()");
}

//______________________________________________________________________________
void TProof::ResetProgressDialog(const char *sel, Int_t sz, Long64_t fst,
                                 Long64_t ent)
{
   // Reset progress dialog.

   PDB(kGlobal,1)
      Info("ResetProgressDialog","(%s,%d,%lld,%lld)", sel, sz, fst, ent);

   EmitVA("ResetProgressDialog(const char*,Int_t,Long64_t,Long64_t)",
          4, sel, sz, fst, ent);
}

//______________________________________________________________________________
void TProof::StartupMessage(const char *msg, Bool_t st, Int_t done, Int_t total)
{
   // Send startup message.

   PDB(kGlobal,1)
      Info("StartupMessage","(%s,%d,%d,%d)", msg, st, done, total);

   EmitVA("StartupMessage(const char*,Bool_t,Int_t,Int_t)",
          4, msg, st, done, total);
}

//______________________________________________________________________________
void TProof::DataSetStatus(const char *msg, Bool_t st, Int_t done, Int_t total)
{
   // Send dataset preparation status.

   PDB(kGlobal,1)
      Info("DataSetStatus","(%s,%d,%d,%d)", msg, st, done, total);

   EmitVA("DataSetStatus(const char*,Bool_t,Int_t,Int_t)",
          4, msg, st, done, total);
}

//______________________________________________________________________________
void TProof::SendDataSetStatus(const char *action, UInt_t done,
                               UInt_t tot, Bool_t st)
{
   // Send or notify data set status

   if (IsLite()) {
      if (tot) {
         TString type = "files";
         Int_t frac = (Int_t) (done*100.)/tot;
         char msg[512] = {0};
         if (frac >= 100) {
            sprintf(msg,"%s: OK (%d %s)                 \n",
                     action,tot, type.Data());
         } else {
            sprintf(msg,"%s: %d out of %d (%d %%)\r",
                     action, done, tot, frac);
         }
         if (fSync)
            fprintf(stderr,"%s", msg);
         else
            NotifyLogMsg(msg, 0);
      }
      return;
   }

   if (TestBit(TProof::kIsMaster)) {
      TMessage mess(kPROOF_DATASET_STATUS);
      mess << TString(action) << tot << done << st;
      gProofServ->GetSocket()->Send(mess);
   }
}

//______________________________________________________________________________
void TProof::QueryResultReady(const char *ref)
{
   // Notify availability of a query result.

   PDB(kGlobal,1)
      Info("QueryResultReady","ref: %s", ref);

   Emit("QueryResultReady(const char*)",ref);
}

//______________________________________________________________________________
void TProof::ValidateDSet(TDSet *dset)
{
   // Validate a TDSet.

   if (dset->ElementsValid()) return;

   TList nodes;
   nodes.SetOwner();

   TList slholder;
   slholder.SetOwner();
   TList elemholder;
   elemholder.SetOwner();

   // build nodelist with slaves and elements
   TIter nextSlave(GetListOfActiveSlaves());
   while (TSlave *sl = dynamic_cast<TSlave*>(nextSlave())) {
      TList *sllist = 0;
      TPair *p = dynamic_cast<TPair*>(nodes.FindObject(sl->GetName()));
      if (!p) {
         sllist = new TList;
         sllist->SetName(sl->GetName());
         slholder.Add(sllist);
         TList *elemlist = new TList;
         elemlist->SetName(TString(sl->GetName())+"_elem");
         elemholder.Add(elemlist);
         nodes.Add(new TPair(sllist, elemlist));
      } else {
         sllist = dynamic_cast<TList*>(p->Key());
      }
      sllist->Add(sl);
   }

   // add local elements to nodes
   TList nonLocal; // list of nonlocal elements
   // make two iterations - first add local elements - then distribute nonlocals
   for (Int_t i = 0; i < 2; i++) {
      Bool_t local = i>0?kFALSE:kTRUE;
      TIter nextElem(local ? dset->GetListOfElements() : &nonLocal);
      while (TDSetElement *elem = dynamic_cast<TDSetElement*>(nextElem())) {
         if (elem->GetValid()) continue;
         TPair *p = dynamic_cast<TPair*>(local?nodes.FindObject(TUrl(elem->GetFileName()).GetHost()):nodes.At(0));
         if (p) {
            TList *eli = dynamic_cast<TList*>(p->Value());
            TList *sli = dynamic_cast<TList*>(p->Key());
            eli->Add(elem);

            // order list by elements/slave
            TPair *p2 = p;
            Bool_t stop = kFALSE;
            while (!stop) {
               TPair *p3 = dynamic_cast<TPair*>(nodes.After(p2->Key()));
               if (p3) {
                  Int_t nelem = dynamic_cast<TList*>(p3->Value())->GetSize();
                  Int_t nsl = dynamic_cast<TList*>(p3->Key())->GetSize();
                  if (nelem*sli->GetSize() < eli->GetSize()*nsl) p2 = p3;
                  else stop = kTRUE;
               } else {
                  stop = kTRUE;
               }
            }

            if (p2!=p) {
               nodes.Remove(p->Key());
               nodes.AddAfter(p2->Key(), p);
            }

         } else {
            if (local) {
               nonLocal.Add(elem);
            } else {
               Error("ValidateDSet", "No Node to allocate TDSetElement to");
               R__ASSERT(0);
            }
         }
      }
   }

   // send to slaves
   TList usedslaves;
   TIter nextNode(&nodes);
   SetDSet(dset); // set dset to be validated in Collect()
   while (TPair *node = dynamic_cast<TPair*>(nextNode())) {
      TList *slaves = dynamic_cast<TList*>(node->Key());
      TList *setelements = dynamic_cast<TList*>(node->Value());

      // distribute elements over the slaves
      Int_t nslaves = slaves->GetSize();
      Int_t nelements = setelements->GetSize();
      for (Int_t i=0; i<nslaves; i++) {

         TDSet copyset(dset->GetType(), dset->GetObjName(),
                       dset->GetDirectory());
         for (Int_t j = (i*nelements)/nslaves;
                    j < ((i+1)*nelements)/nslaves;
                    j++) {
            TDSetElement *elem =
               dynamic_cast<TDSetElement*>(setelements->At(j));
            copyset.Add(elem->GetFileName(), elem->GetObjName(),
                        elem->GetDirectory(), elem->GetFirst(),
                        elem->GetNum(), elem->GetMsd());
         }

         if (copyset.GetListOfElements()->GetSize()>0) {
            TMessage mesg(kPROOF_VALIDATE_DSET);
            mesg << &copyset;

            TSlave *sl = dynamic_cast<TSlave*>(slaves->At(i));
            PDB(kGlobal,1) Info("ValidateDSet",
                                "Sending TDSet with %d elements to slave %s"
                                " to be validated",
                                copyset.GetListOfElements()->GetSize(),
                                sl->GetOrdinal());
            sl->GetSocket()->Send(mesg);
            usedslaves.Add(sl);
         }
      }
   }

   PDB(kGlobal,1)
      Info("ValidateDSet","Calling Collect");
   Collect(&usedslaves);
   SetDSet(0);
}

//______________________________________________________________________________
void TProof::AddInputData(TObject *obj, Bool_t push)
{
   // Add data objects that might be needed during the processing of
   // the selector (see Process()). This object can be very large, so they
   // are distributed in an optimized way using a dedicated file.
   // If push is TRUE the input data are sent over even if no apparent change
   // occured to the list.

   if (obj) {
      if (!fInputData) fInputData = new TList;
      if (!fInputData->FindObject(obj)) {
         fInputData->Add(obj);
         SetBit(TProof::kNewInputData);
      }
   }
   if (push) SetBit(TProof::kNewInputData);
}

//______________________________________________________________________________
void TProof::ClearInputData(TObject *obj)
{
   // Remove obj form the input data list; if obj is null (default), clear the
   // input data info.

   if (!obj) {
      if (fInputData) {
         fInputData->SetOwner(kTRUE);
         SafeDelete(fInputData);
      }
      ResetBit(TProof::kNewInputData);

      // Also remove any info about input data in the input list
      TObject *o = 0;
      TList *in = GetInputList();
      while ((o = GetInputList()->FindObject("PROOF_InputDataFile")))
         in->Remove(o);
      while ((o = GetInputList()->FindObject("PROOF_InputData")))
         in->Remove(o);

      // ... and reset the file
      fInputDataFile = "";
      gSystem->Unlink(kPROOF_InputDataFile);

   } else if (fInputData) {
      Int_t sz = fInputData->GetSize();
      while (fInputData->FindObject(obj))
         fInputData->Remove(obj);
      // Flag for update, if anything changed
      if (sz != fInputData->GetSize())
         SetBit(TProof::kNewInputData);
   }
}

//______________________________________________________________________________
void TProof::ClearInputData(const char *name)
{
   // Remove obj 'name' form the input data list;

   TObject *obj = (fInputData && name) ? fInputData->FindObject(name) : 0;
   if (obj) ClearInputData(obj);
}

//______________________________________________________________________________
void TProof::SetInputDataFile(const char *datafile)
{
   // Set the file to be used to optimally distribute the input data objects.
   // If the file exists the object in the file are added to those in the
   // fInputData list. If the file path is null, a default file will be created
   // at the moment of sending the processing request with the content of
   // the fInputData list. See also SendInputDataFile.

   if (datafile && strlen(datafile) > 0) {
      if (fInputDataFile != datafile && strcmp(datafile, kPROOF_InputDataFile))
         SetBit(TProof::kNewInputData);
      fInputDataFile = datafile;
   } else {
      if (!fInputDataFile.IsNull())
         SetBit(TProof::kNewInputData);
      fInputDataFile = "";
   }
   // Make sure that the chosen file is readable
   if (fInputDataFile != kPROOF_InputDataFile && !fInputDataFile.IsNull() &&
      gSystem->AccessPathName(fInputDataFile, kReadPermission)) {
      fInputDataFile = "";
   }
}

//______________________________________________________________________________
void TProof::SendInputDataFile()
{
   // Send the input data objects to the master; the objects are taken from the
   // dedicated list and / or the specified file.
   // If the fInputData is empty the specified file is sent over.
   // If there is no specified file, a file named "inputdata.root" is created locally
   // with the content of fInputData and sent over to the master.
   // If both fInputData and the specified file are not empty, a copy of the file
   // is made locally and augmented with the content of fInputData.

   // Prepare the file
   TString dataFile;
   PrepareInputDataFile(dataFile);

   // Send it, if not empty
   if (dataFile.Length() > 0) {

      Info("SendInputDataFile", "broadcasting %s", dataFile.Data());
      BroadcastFile(dataFile.Data(), kBinary, "cache", kActive);

      // Set the name in the input list
      AddInput(new TNamed("PROOF_InputDataFile", Form("cache:%s", gSystem->BaseName(dataFile))));
   }
}

//______________________________________________________________________________
void TProof::PrepareInputDataFile(TString &dataFile)
{
   // Prepare the file with the input data objects to be sent the master; the
   // objects are taken from the dedicated list and / or the specified file.
   // If the fInputData is empty the specified file is sent over.
   // If there is no specified file, a file named "inputdata.root" is created locally
   // with the content of fInputData and sent over to the master.
   // If both fInputData and the specified file are not empty, a copy of the file
   // is made locally and augmented with the content of fInputData.

   // Save info about new data for usage in this call;
   Bool_t newdata = TestBit(TProof::kNewInputData) ? kTRUE : kFALSE;
   // Next time we need some change
   ResetBit(TProof::kNewInputData);

   // Check the list
   Bool_t list_ok = (fInputData && fInputData->GetSize() > 0) ? kTRUE : kFALSE;
   // Check the file
   Bool_t file_ok = kFALSE;
   if (fInputDataFile != kPROOF_InputDataFile && !fInputDataFile.IsNull() &&
      !gSystem->AccessPathName(fInputDataFile, kReadPermission)) {
      // It must contain something
      TFile *f = TFile::Open(fInputDataFile);
      if (f && f->GetListOfKeys() && f->GetListOfKeys()->GetSize() > 0)
         file_ok = kTRUE;
   }

   // Remove any info about input data in the input list
   TObject *o = 0;
   TList *in = GetInputList();
   while ((o = GetInputList()->FindObject("PROOF_InputDataFile")))
      in->Remove(o);
   while ((o = GetInputList()->FindObject("PROOF_InputData")))
      in->Remove(o);

   // We must have something to send
   dataFile = "";
   if (!list_ok && !file_ok) return;

   // Three cases:
   if (file_ok && !list_ok) {
      // Just send the file
      dataFile = fInputDataFile;
   } else if (!file_ok && list_ok) {
      fInputDataFile = kPROOF_InputDataFile;
      // Nothing to do, if no new data
      if (!newdata && !gSystem->AccessPathName(fInputDataFile)) return;
      // Create the file first
      TFile *f = TFile::Open(fInputDataFile, "RECREATE");
      if (f) {
         f->cd();
         TIter next(fInputData);
         TObject *obj;
         while ((obj = next())) {
            obj->Write(0, TObject::kSingleKey, 0);
         }
         f->Close();
         SafeDelete(f);
      } else {
         Error("PrepareInputDataFile", "could not (re-)create %s", fInputDataFile.Data());
         return;
      }
      dataFile = fInputDataFile;
   } else if (file_ok && list_ok) {
      dataFile = kPROOF_InputDataFile;
      // Create the file if not existing or there are new data
      if (newdata || gSystem->AccessPathName(dataFile)) {
         // Cleanup previous file if obsolete
         if (!gSystem->AccessPathName(dataFile))
            gSystem->Unlink(dataFile);
         if (dataFile != fInputDataFile) {
            // Make a local copy first
            if (gSystem->CopyFile(fInputDataFile, dataFile, kTRUE) != 0) {
               Error("PrepareInputDataFile", "could not make local copy of %s", fInputDataFile.Data());
               return;
            }
         }
         // Add the input data list
         TFile *f = TFile::Open(dataFile, "UPDATE");
         if (f) {
            f->cd();
            TIter next(fInputData);
            TObject *obj = 0;
            while ((obj = next())) {
               obj->Write(0, TObject::kSingleKey, 0);
            }
            f->Close();
            SafeDelete(f);
         } else {
            Error("PrepareInputDataFile", "could not open %s for updating", dataFile.Data());
            return;
         }
      }
   }

   //  Done
   return;
}

//______________________________________________________________________________
void TProof::AddInput(TObject *obj)
{
   // Add objects that might be needed during the processing of
   // the selector (see Process()).

   if (fPlayer) fPlayer->AddInput(obj);
}

//______________________________________________________________________________
void TProof::ClearInput()
{
   // Clear input object list.

   if (fPlayer) fPlayer->ClearInput();

   // the system feedback list is always in the input list
   AddInput(fFeedback);
}

//______________________________________________________________________________
TList *TProof::GetInputList()
{
   // Get input list.

   return (fPlayer ? fPlayer->GetInputList() : (TList *)0);
}

//______________________________________________________________________________
TObject *TProof::GetOutput(const char *name)
{
   // Get specified object that has been produced during the processing
   // (see Process()).

   // Can be called by MarkBad on the master before the player is initialized
   return (fPlayer) ? fPlayer->GetOutput(name) : (TObject *)0;
}

//______________________________________________________________________________
TList *TProof::GetOutputList()
{
   // Get list with all object created during processing (see Process()).

   return (fPlayer ? fPlayer->GetOutputList() : (TList *)0);
}

//______________________________________________________________________________
void TProof::SetParameter(const char *par, const char *value)
{
   // Set input list parameter. If the parameter is already
   // set it will be set to the new value.

   if (!fPlayer) {
      Warning("SetParameter", "player undefined! Ignoring");
      return;
   }

   TList *il = fPlayer->GetInputList();
   TObject *item = il->FindObject(par);
   if (item) {
      il->Remove(item);
      delete item;
   }
   il->Add(new TNamed(par, value));
}

//______________________________________________________________________________
void TProof::SetParameter(const char *par, Int_t value)
{
   // Set an input list parameter.

   if (!fPlayer) {
      Warning("SetParameter", "player undefined! Ignoring");
      return;
   }

   TList *il = fPlayer->GetInputList();
   TObject *item = il->FindObject(par);
   if (item) {
      il->Remove(item);
      delete item;
   }
   il->Add(new TParameter<Int_t>(par, value));
}

//______________________________________________________________________________
void TProof::SetParameter(const char *par, Long_t value)
{
   // Set an input list parameter.

   if (!fPlayer) {
      Warning("SetParameter", "player undefined! Ignoring");
      return;
   }

   TList *il = fPlayer->GetInputList();
   TObject *item = il->FindObject(par);
   if (item) {
      il->Remove(item);
      delete item;
   }
   il->Add(new TParameter<Long_t>(par, value));
}

//______________________________________________________________________________
void TProof::SetParameter(const char *par, Long64_t value)
{
   // Set an input list parameter.

   if (!fPlayer) {
      Warning("SetParameter", "player undefined! Ignoring");
      return;
   }

   TList *il = fPlayer->GetInputList();
   TObject *item = il->FindObject(par);
   if (item) {
      il->Remove(item);
      delete item;
   }
   il->Add(new TParameter<Long64_t>(par, value));
}

//______________________________________________________________________________
void TProof::SetParameter(const char *par, Double_t value)
{
   // Set an input list parameter.

   if (!fPlayer) {
      Warning("SetParameter", "player undefined! Ignoring");
      return;
   }

   TList *il = fPlayer->GetInputList();
   TObject *item = il->FindObject(par);
   if (item) {
      il->Remove(item);
      delete item;
   }
   il->Add(new TParameter<Double_t>(par, value));
}

//______________________________________________________________________________
TObject *TProof::GetParameter(const char *par) const
{
   // Get specified parameter. A parameter set via SetParameter() is either
   // a TParameter or a TNamed or 0 in case par is not defined.

   if (!fPlayer) {
      Warning("GetParameter", "player undefined! Ignoring");
      return (TObject *)0;
   }

   TList *il = fPlayer->GetInputList();
   return il->FindObject(par);
}

//______________________________________________________________________________
void TProof::DeleteParameters(const char *wildcard)
{
   // Delete the input list parameters specified by a wildcard (e.g. PROOF_*)
   // or exact name (e.g. PROOF_MaxSlavesPerNode).

   if (!fPlayer) return;

   if (!wildcard) wildcard = "";
   TRegexp re(wildcard, kTRUE);
   Int_t nch = strlen(wildcard);

   TList *il = fPlayer->GetInputList();
   TObject *p;
   TIter next(il);
   while ((p = next())) {
      TString s = p->GetName();
      if (nch && s != wildcard && s.Index(re) == kNPOS) continue;
      il->Remove(p);
      delete p;
   }
}

//______________________________________________________________________________
void TProof::ShowParameters(const char *wildcard) const
{
   // Show the input list parameters specified by the wildcard.
   // Default is the special PROOF control parameters (PROOF_*).

   if (!fPlayer) return;

   if (!wildcard) wildcard = "";
   TRegexp re(wildcard, kTRUE);
   Int_t nch = strlen(wildcard);

   TList *il = fPlayer->GetInputList();
   TObject *p;
   TIter next(il);
   while ((p = next())) {
      TString s = p->GetName();
      if (nch && s != wildcard && s.Index(re) == kNPOS) continue;
      if (p->IsA() == TNamed::Class()) {
         Printf("%s\t\t\t%s", s.Data(), p->GetTitle());
      } else if (p->IsA() == TParameter<Long_t>::Class()) {
         Printf("%s\t\t\t%ld", s.Data(), dynamic_cast<TParameter<Long_t>*>(p)->GetVal());
      } else if (p->IsA() == TParameter<Long64_t>::Class()) {
         Printf("%s\t\t\t%lld", s.Data(), dynamic_cast<TParameter<Long64_t>*>(p)->GetVal());
      } else if (p->IsA() == TParameter<Double_t>::Class()) {
         Printf("%s\t\t\t%f", s.Data(), dynamic_cast<TParameter<Double_t>*>(p)->GetVal());
      } else {
         Printf("%s\t\t\t%s", s.Data(), p->GetTitle());
      }
   }
}

//______________________________________________________________________________
void TProof::AddFeedback(const char *name)
{
   // Add object to feedback list.

   PDB(kFeedback, 3)
      Info("AddFeedback", "Adding object \"%s\" to feedback", name);
   if (fFeedback->FindObject(name) == 0)
      fFeedback->Add(new TObjString(name));
}

//______________________________________________________________________________
void TProof::RemoveFeedback(const char *name)
{
   // Remove object from feedback list.

   TObject *obj = fFeedback->FindObject(name);
   if (obj != 0) {
      fFeedback->Remove(obj);
      delete obj;
   }
}

//______________________________________________________________________________
void TProof::ClearFeedback()
{
   // Clear feedback list.

   fFeedback->Delete();
}

//______________________________________________________________________________
void TProof::ShowFeedback() const
{
   // Show items in feedback list.

   if (fFeedback->GetSize() == 0) {
      Info("","no feedback requested");
      return;
   }

   fFeedback->Print();
}

//______________________________________________________________________________
TList *TProof::GetFeedbackList() const
{
   // Return feedback list.

   return fFeedback;
}

//______________________________________________________________________________
TTree *TProof::GetTreeHeader(TDSet *dset)
{
   // Creates a tree header (a tree with nonexisting files) object for
   // the DataSet.

   TList *l = GetListOfActiveSlaves();
   TSlave *sl = (TSlave*) l->First();
   if (sl == 0) {
      Error("GetTreeHeader", "No connection");
      return 0;
   }

   TSocket *soc = sl->GetSocket();
   TMessage msg(kPROOF_GETTREEHEADER);

   msg << dset;

   soc->Send(msg);

   TMessage *reply;
   Int_t d = -1;
   if (fProtocol >= 20) {
      Collect(sl, fCollectTimeout, kPROOF_GETTREEHEADER);
      reply = (TMessage *) fRecvMessages->First();
   } else {
      d = soc->Recv(reply);
   }
   if (!reply) {
      Error("GetTreeHeader", "Error getting a replay from the master.Result %d", (int) d);
      return 0;
   }

   TString s1;
   TTree *t = 0;
   (*reply) >> s1;
   if (s1 == "Success")
      (*reply) >> t;

   PDB(kGlobal, 1) {
      if (t) {
         Info("GetTreeHeader", "%s, message size: %d, entries: %d",
                               s1.Data(), reply->BufferSize(), (int) t->GetMaxEntryLoop());
      } else {
         Info("GetTreeHeader", "tree header retrieval failed");
      }
   }
   delete reply;

   return t;
}

//______________________________________________________________________________
TDrawFeedback *TProof::CreateDrawFeedback()
{
   // Draw feedback creation proxy. When accessed via TProof avoids
   // link dependency on libProofPlayer.

   return (fPlayer ? fPlayer->CreateDrawFeedback(this) : (TDrawFeedback *)0);
}

//______________________________________________________________________________
void TProof::SetDrawFeedbackOption(TDrawFeedback *f, Option_t *opt)
{
   // Set draw feedback option.

   if (fPlayer) fPlayer->SetDrawFeedbackOption(f, opt);
}

//______________________________________________________________________________
void TProof::DeleteDrawFeedback(TDrawFeedback *f)
{
   // Delete draw feedback object.

   if (fPlayer) fPlayer->DeleteDrawFeedback(f);
}

//______________________________________________________________________________
TList *TProof::GetOutputNames()
{
   //   FIXME: to be written

   return 0;
/*
   TMessage msg(kPROOF_GETOUTPUTLIST);
   TList* slaves = fActiveSlaves;
   Broadcast(msg, slaves);
   TMonitor mon;
   TList* outputList = new TList();

   TIter    si(slaves);
   TSlave   *slave;
   while ((slave = (TSlave*)si.Next()) != 0) {
      PDB(kGlobal,4) Info("GetOutputNames","Socket added to monitor: %p (%s)",
          slave->GetSocket(), slave->GetName());
      mon.Add(slave->GetSocket());
   }
   mon.ActivateAll();
   ((TProof*)gProof)->DeActivateAsyncInput();
   ((TProof*)gProof)->fCurrentMonitor = &mon;

   while (mon.GetActive() != 0) {
      TSocket *sock = mon.Select();
      if (!sock) {
         Error("GetOutputList","TMonitor::.Select failed!");
         break;
      }
      mon.DeActivate(sock);
      TMessage *reply;
      if (sock->Recv(reply) <= 0) {
         MarkBad(slave, "receive failed after kPROOF_GETOUTPUTLIST request");
//         Error("GetOutputList","Recv failed! for slave-%d (%s)",
//               slave->GetOrdinal(), slave->GetName());
         continue;
      }
      if (reply->What() != kPROOF_GETOUTPUTNAMES ) {
//         Error("GetOutputList","unexpected message %d from slawe-%d (%s)",  reply->What(),
//               slave->GetOrdinal(), slave->GetName());
         MarkBad(slave, "wrong reply to kPROOF_GETOUTPUTLIST request");
         continue;
      }
      TList* l;

      (*reply) >> l;
      TIter next(l);
      TNamed *n;
      while ( (n = dynamic_cast<TNamed*> (next())) ) {
         if (!outputList->FindObject(n->GetName()))
            outputList->Add(n);
      }
      delete reply;
   }
   ((TProof*)gProof)->fCurrentMonitor = 0;

   return outputList;
*/
}

//______________________________________________________________________________
void TProof::Browse(TBrowser *b)
{
   // Build the PROOF's structure in the browser.

   b->Add(fActiveSlaves, fActiveSlaves->Class(), "fActiveSlaves");
   b->Add(&fMaster, fMaster.Class(), "fMaster");
   b->Add(fFeedback, fFeedback->Class(), "fFeedback");
   b->Add(fChains, fChains->Class(), "fChains");

   if (fPlayer) {
      b->Add(fPlayer->GetInputList(), fPlayer->GetInputList()->Class(), "InputList");
      if (fPlayer->GetOutputList())
         b->Add(fPlayer->GetOutputList(), fPlayer->GetOutputList()->Class(), "OutputList");
      if (fPlayer->GetListOfResults())
         b->Add(fPlayer->GetListOfResults(),
               fPlayer->GetListOfResults()->Class(), "ListOfResults");
   }
}

//______________________________________________________________________________
void TProof::SetPlayer(TVirtualProofPlayer *player)
{
   // Set a new PROOF player.

   if (fPlayer)
      delete fPlayer;
   fPlayer = player;
};

//______________________________________________________________________________
TVirtualProofPlayer *TProof::MakePlayer(const char *player, TSocket *s)
{
   // Construct a TProofPlayer object. The player string specifies which
   // player should be created: remote, slave, sm (supermaster) or base.
   // Default is remote. Socket is needed in case a slave player is created.

   if (!player)
      player = "remote";

   SetPlayer(TVirtualProofPlayer::Create(player, this, s));
   return GetPlayer();
}

//______________________________________________________________________________
void TProof::AddChain(TChain *chain)
{
   // Add chain to data set

   fChains->Add(chain);
}

//______________________________________________________________________________
void TProof::RemoveChain(TChain *chain)
{
   // Remove chain from data set

   fChains->Remove(chain);
}

//______________________________________________________________________________
void TProof::GetLog(Int_t start, Int_t end)
{
   // Ask for remote logs in the range [start, end]. If start == -1 all the
   // messages not yet received are sent back.

   if (!IsValid() || TestBit(TProof::kIsMaster)) return;

   TMessage msg(kPROOF_LOGFILE);

   msg << start << end;

   Broadcast(msg, kActive);
   Collect(kActive, fCollectTimeout);
}

//______________________________________________________________________________
void TProof::PutLog(TQueryResult *pq)
{
   // Display log of query pq into the log window frame

   if (!pq) return;

   TList *lines = pq->GetLogFile()->GetListOfLines();
   if (lines) {
      TIter nxl(lines);
      TObjString *l = 0;
      while ((l = (TObjString *)nxl()))
         EmitVA("LogMessage(const char*,Bool_t)", 2, l->GetName(), kFALSE);
   }
}

//______________________________________________________________________________
void TProof::ShowLog(const char *queryref)
{
   // Display on screen the content of the temporary log file for query
   // in reference

   // Make sure we have all info (GetListOfQueries retrieves the
   // head info only)
   Retrieve(queryref);

   if (fPlayer) {
      if (queryref) {
         if (fPlayer->GetListOfResults()) {
            TIter nxq(fPlayer->GetListOfResults());
            TQueryResult *qr = 0;
            while ((qr = (TQueryResult *) nxq()))
               if (strstr(queryref, qr->GetTitle()) &&
                   strstr(queryref, qr->GetName()))
                  break;
            if (qr) {
               PutLog(qr);
               return;
            }

         }
      }
   }
}

//______________________________________________________________________________
void TProof::ShowLog(Int_t qry)
{
   // Display on screen the content of the temporary log file.
   // If qry == -2 show messages from the last (current) query.
   // If qry == -1 all the messages not yet displayed are shown (default).
   // If qry == 0, all the messages in the file are shown.
   // If qry  > 0, only the messages related to query 'qry' are shown.
   // For qry != -1 the original file offset is restored at the end

   // Save present offset
   Int_t nowlog = lseek(fileno(fLogFileR), (off_t) 0, SEEK_CUR);

   // Get extremes
   Int_t startlog = nowlog;
   Int_t endlog = lseek(fileno(fLogFileR), (off_t) 0, SEEK_END);

   lseek(fileno(fLogFileR), (off_t) nowlog, SEEK_SET);
   if (qry == 0) {
      startlog = 0;
      lseek(fileno(fLogFileR), (off_t) 0, SEEK_SET);
   } else if (qry != -1) {

      TQueryResult *pq = 0;
      if (qry == -2) {
         // Pickup the last one
         pq = (GetQueryResults()) ? ((TQueryResult *)(GetQueryResults()->Last())) : 0;
         if (!pq) {
            GetListOfQueries();
            if (fQueries)
               pq = (TQueryResult *)(fQueries->Last());
         }
      } else if (qry > 0) {
         TList *queries = GetQueryResults();
         if (queries) {
            TIter nxq(queries);
            while ((pq = (TQueryResult *)nxq()))
               if (qry == pq->GetSeqNum())
                  break;
         }
         if (!pq) {
            queries = GetListOfQueries();
            TIter nxq(queries);
            while ((pq = (TQueryResult *)nxq()))
               if (qry == pq->GetSeqNum())
                  break;
         }
      }
      if (pq) {
         PutLog(pq);
         return;
      } else {
         if (gDebug > 0)
            Info("ShowLog","query %d not found in list", qry);
         qry = -1;
      }
   }

   // Number of bytes to log
   UInt_t tolog = (UInt_t)(endlog - startlog);

   // Perhaps nothing
   if (tolog <= 0)

   // Set starting point
   lseek(fileno(fLogFileR), (off_t) startlog, SEEK_SET);

   // Now we go
   Int_t np = 0;
   char line[2048];
   Int_t wanted = (tolog > sizeof(line)) ? sizeof(line) : tolog;
   while (fgets(line, wanted, fLogFileR)) {

      Int_t r = strlen(line);
      if (!SendingLogToWindow()) {
         if (line[r-1] != '\n') line[r-1] = '\n';
         if (r > 0) {
            char *p = line;
            while (r) {
               Int_t w = write(fileno(stdout), p, r);
               if (w < 0) {
                  SysError("ShowLogFile", "error writing to stdout");
                  break;
               }
               r -= w;
               p += w;
            }
         }
         tolog -= strlen(line);
         np++;

         // Ask if more is wanted
         if (!(np%10)) {
            char *opt = Getline("More (y/n)? [y]");
            if (opt[0] == 'n')
               break;
         }

         // We may be over
         if (tolog <= 0)
            break;

         // Update wanted bytes
         wanted = (tolog > sizeof(line)) ? sizeof(line) : tolog;
      } else {
         // Log to window
         if (line[r-1] == '\n') line[r-1] = 0;
         LogMessage(line, kFALSE);
      }
   }
   if (!SendingLogToWindow()) {
      // Avoid screwing up the prompt
      if (write(fileno(stdout), "\n", 1) != 1)
         SysError("ShowLogFile", "error writing to stdout");
   }

   // Restore original pointer
   if (qry > -1)
      lseek(fileno(fLogFileR), (off_t) nowlog, SEEK_SET);
}

//______________________________________________________________________________
void TProof::cd(Int_t id)
{
   // Set session with 'id' the default one. If 'id' is not found in the list,
   // the current session is set as default

   if (GetManager()) {
      TProofDesc *d = GetManager()->GetProofDesc(id);
      if (d) {
         if (d->GetProof()) {
            gProof = d->GetProof();
            return;
         }
      }

      // Id not found or undefined: set as default this session
      gProof = this;
   }

   return;
}

//______________________________________________________________________________
void TProof::Detach(Option_t *opt)
{
   // Detach this instance to its proofserv.
   // If opt is 'S' or 's' the remote server is shutdown

   // Nothing to do if not in contact with proofserv
   if (!IsValid()) return;

   // Get worker and socket instances
   TSlave *sl = (TSlave *) fActiveSlaves->First();
   TSocket *s = 0;
   if (!sl || !(sl->IsValid()) || !(s = sl->GetSocket())) {
      Error("Detach","corrupted worker instance: wrk:%p, sock:%p", sl, s);
      return;
   }

   Bool_t shutdown = (strchr(opt,'s') || strchr(opt,'S')) ? kTRUE : kFALSE;

   // If processing, try to stop processing first
   if (shutdown && !IsIdle()) {
      // Remove pending requests
      Remove("cleanupqueue");
      // Do not wait for ever, but al least 20 seconds
      Long_t timeout = gEnv->GetValue("Proof.ShutdownTimeout", 60);
      timeout = (timeout > 20) ? timeout : 20;
      // Send stop signal
      StopProcess(kFALSE, (Long_t) (timeout / 2));
      // Receive results
      Collect(kActive, timeout);
   }

   // Avoid spurious messages: deactivate new inputs ...
   DeActivateAsyncInput();

   // ... and discard existing ones
   sl->FlushSocket();

   // Close session (we always close the connection)
   Close(opt);

   // Close the progress dialog, if any
   if (fProgressDialogStarted)
      CloseProgressDialog();

   // Update info in the table of our manager, if any
   if (GetManager() && GetManager()->QuerySessions("L")) {
      TIter nxd(GetManager()->QuerySessions("L"));
      TProofDesc *d = 0;
      while ((d = (TProofDesc *)nxd())) {
         if (d->GetProof() == this) {
            d->SetProof(0);
            GetManager()->QuerySessions("L")->Remove(d);
            break;
         }
      }
   }

   // Delete this instance
   if ((!fProgressDialogStarted) && !TestBit(kUsingSessionGui))
      delete this;
   else
      // ~TProgressDialog will delete this
      fValid = kFALSE;

   return;
}

//______________________________________________________________________________
void TProof::SetAlias(const char *alias)
{
   // Set an alias for this session. If reconnection is supported, the alias
   // will be communicated to the remote coordinator so that it can be recovered
   // when reconnecting

   // Set it locally
   TNamed::SetTitle(alias);
   if (TestBit(TProof::kIsMaster))
      // Set the name at the same value
      TNamed::SetName(alias);

   // Nothing to do if not in contact with coordinator
   if (!IsValid()) return;

   if (!IsProofd() && TestBit(TProof::kIsClient)) {
      TSlave *sl = (TSlave *) fActiveSlaves->First();
      if (sl)
         sl->SetAlias(alias);
   }

   return;
}

//______________________________________________________________________________
Int_t TProof::UploadDataSet(const char *dataSetName,
                            TList *files,
                            const char *desiredDest,
                            Int_t opt,
                            TList *skippedFiles)
{
   // Upload a set of files and save the list of files by name dataSetName.
   // The 'files' argument is a list of TFileInfo objects describing the files
   // as first url.
   // The mask 'opt' is a combination of EUploadOpt:
   //   kAppend             (0x1)   if set true files will be appended to
   //                               the dataset existing by given name
   //   kOverwriteDataSet   (0x2)   if dataset with given name exited it
   //                               would be overwritten
   //   kNoOverwriteDataSet (0x4)   do not overwirte if the dataset exists
   //   kOverwriteAllFiles  (0x8)   overwrite all files that may exist
   //   kOverwriteNoFiles   (0x10)  overwrite none
   //   kAskUser            (0x0)   ask user before overwriteng dataset/files
   // The default value is kAskUser.
   // The user will be asked to confirm overwriting dataset or files unless
   // specified opt provides the answer!
   // If kOverwriteNoFiles is set, then a pointer to TList must be passed as
   // skippedFiles argument. The function will add to this list TFileInfo
   // objects describing all files that existed on the cluster and were
   // not uploaded.
   //
   // Communication Summary
   // Client                             Master
   //    |------------>DataSetName----------->|
   //    |<-------kMESS_OK/kMESS_NOTOK<-------| (Name OK/file exist)
   // (*)|-------> call RegisterDataSet ------->|
   // (*) - optional

   if (fProtocol < 15) {
      Info("UploadDataSet", "functionality not available: the server has an"
                            " incompatible version of TFileInfo");
      return -1;
   }

   if (IsLite()) {
      Info("UploadDataSet", "Lite-session: functionality not needed - do nothing");
      return -1;
   }

   // check if  dataSetName is not excluded
   if (strchr(dataSetName, '/')) {
      if (strstr(dataSetName, "public") != dataSetName) {
         Error("UploadDataSet",
               "Name of public dataset should start with public/");
         return kError;
      }
   }
   if ((opt & kOverwriteAllFiles && opt & kOverwriteNoFiles) ||
       (opt & kNoOverwriteDataSet && opt & kAppend) ||
       (opt & kOverwriteDataSet && opt & kAppend) ||
       (opt & kNoOverwriteDataSet && opt & kOverwriteDataSet) ||
       (opt & kAskUser && opt & (kOverwriteDataSet | kNoOverwriteDataSet | kAppend |
                                 kOverwriteAllFiles | kOverwriteNoFiles))) {
      Error("UploadDataSet", "you specified contradicting options.");
      return kError;
   }

   // Decode options
   Int_t overwriteAll = (opt & kOverwriteAllFiles) ? kTRUE : kFALSE;
   Int_t overwriteNone = (opt & kOverwriteNoFiles) ? kTRUE : kFALSE;
   Int_t goodName = (opt & (kOverwriteDataSet | kAppend)) ? 1 : -1;
   Int_t appendToDataSet = (opt & kAppend) ? kTRUE : kFALSE;
   Int_t overwriteNoDataSet = (opt & kNoOverwriteDataSet) ? kTRUE : kFALSE;


   //If skippedFiles is not provided we can not return list of skipped files.
   if (!skippedFiles && overwriteNone) {
      Error("UploadDataSet",
            "Provide pointer to TList object as skippedFiles argument when using kOverwriteNoFiles option.");
      return kError;
   }
   //If skippedFiles is provided but did not point to a TList the have to STOP
   if (skippedFiles) {
      if (skippedFiles->Class() != TList::Class()) {
         Error("UploadDataSet",
               "Provided skippedFiles argument does not point to a TList object.");
         return kError;
      }
   }
   TSocket *master;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("UploadDataSet", "No connection to the master!");
      return kError;
   }

   Int_t fileCount = 0; // return value
   if (goodName == -1) { // -1 for undefined
      // First check whether this dataset already exists unless
      // kAppend or kOverWriteDataSet
      TMessage nameMess(kPROOF_DATASETS);
      nameMess << Int_t(kCheckDataSetName);
      nameMess << TString(dataSetName);
      Broadcast(nameMess);
      Collect(kActive, fCollectTimeout); //after each call to HandleDataSets
      if (fStatus == -1) {
         //We ask user to agree on overwriting the dataset name
         while (goodName == -1 && !overwriteNoDataSet) {
            Info("UploadDataSet", "dataset %s already exist. ",
                   dataSetName);
            Info("UploadDataSet", "do you want to overwrite it[Yes/No/Append]?");
            TString answer;
            answer.ReadToken(cin);
            if (!strncasecmp(answer.Data(), "y", 1)) {
               goodName = 1;
            } else if (!strncasecmp(answer.Data(), "n", 1)) {
               goodName = 0;
            } else if (!strncasecmp(answer.Data(), "a", 1)) {
               goodName = 1;
               appendToDataSet = kTRUE;
            }
         }
      } else {
         goodName = 1;
      }
   } // if (goodName == -1)
   if (goodName == 1) {  //must be == 1 as -1 was used for a bad name!
      //Code for enforcing writing in user "home dir" only
      char *relativeDestDir = Form("%s/%s/",
                                   gSystem->GetUserInfo()->fUser.Data(),
                                   desiredDest?desiredDest:"");
                                   //Consider adding dataSetName to the path

      relativeDestDir = CollapseSlashesInPath(relativeDestDir);
      TString dest = Form("%s/%s", GetDataPoolUrl(), relativeDestDir);

      delete[] relativeDestDir;

      // Now we will actually copy files and create the TList object
      TFileCollection *fileList = new TFileCollection();
      TIter next(files);
      while (TFileInfo *fileInfo = ((TFileInfo*)next())) {
         TUrl *fileUrl = fileInfo->GetFirstUrl();
         if (gSystem->AccessPathName(fileUrl->GetUrl()) == kFALSE) {
            //matching dir entry
            //getting the file name from the path represented by fileUrl
            const char *ent = gSystem->BaseName(fileUrl->GetFile());

            Int_t goodFileName = 1;
            if (!overwriteAll &&
               gSystem->AccessPathName(Form("%s/%s", dest.Data(), ent), kFileExists)
                  == kFALSE) {  //Destination file exists
               goodFileName = -1;
               while (goodFileName == -1 && !overwriteAll && !overwriteNone) {
                  Info("UploadDataSet", "file %s already exists. ", Form("%s/%s", dest.Data(), ent));
                  Info("UploadDataSet", "do you want to overwrite it [Yes/No/all/none]?");
                  TString answer;
                  answer.ReadToken(cin);
                  if (!strncasecmp(answer.Data(), "y", 1))
                     goodFileName = 1;
                  else if (!strncasecmp(answer.Data(), "all", 3))
                     overwriteAll = kTRUE;
                  else if (!strncasecmp(answer.Data(), "none", 4))
                     overwriteNone = kTRUE;
                  else if (!strncasecmp(answer.Data(), "n", 1))
                     goodFileName = 0;
               }
            } //if file exists

            // Copy the file to the redirector indicated
            if (goodFileName == 1 || overwriteAll) {
               //must be == 1 as -1 was meant for bad name!
               Info("UploadDataSet", "Uploading %s to %s/%s",
                      fileUrl->GetUrl(), dest.Data(), ent);
               if (TFile::Cp(fileUrl->GetUrl(), Form("%s/%s", dest.Data(), ent))) {
                  fileList->GetList()->Add(new TFileInfo(Form("%s/%s", dest.Data(), ent)));
               } else
                  Error("UploadDataSet", "file %s was not copied", fileUrl->GetUrl());
            } else {  // don't overwrite, but file exist and must be included
               fileList->GetList()->Add(new TFileInfo(Form("%s/%s", dest.Data(), ent)));
               if (skippedFiles) {
                  // user specified the TList *skippedFiles argument so we create
                  // the list of skipped files
                  skippedFiles->Add(new TFileInfo(fileUrl->GetUrl()));
               }
            }
         } //if matching dir entry
      } //while

      if ((fileCount = fileList->GetList()->GetSize()) == 0) {
         Info("UploadDataSet", "no files were copied. The dataset will not be saved");
      } else {
         TString o = (appendToDataSet) ? "" : "O";
         if (!RegisterDataSet(dataSetName, fileList, o)) {
            Error("UploadDataSet", "Error while saving dataset: %s", dataSetName);
            fileCount = kError;
         }
      }
      delete fileList;
   } else if (overwriteNoDataSet) {
      Info("UploadDataSet", "dataset %s already exists", dataSetName);
      return kDataSetExists;
   } //if(goodName == 1)

   return fileCount;
}

//______________________________________________________________________________
Int_t TProof::UploadDataSet(const char *dataSetName,
                            const char *files,
                            const char *desiredDest,
                            Int_t opt,
                            TList *skippedFiles)
{
   // Upload a set of files and save the list of files by name dataSetName.
   // The mask 'opt' is a combination of EUploadOpt:
   //   kAppend             (0x1)   if set true files will be appended to
   //                               the dataset existing by given name
   //   kOverwriteDataSet   (0x2)   if dataset with given name exited it
   //                               would be overwritten
   //   kNoOverwriteDataSet (0x4)   do not overwirte if the dataset exists
   //   kOverwriteAllFiles  (0x8)   overwrite all files that may exist
   //   kOverwriteNoFiles   (0x10)  overwrite none
   //   kAskUser            (0x0)   ask user before overwriteng dataset/files
   // The default value is kAskUser.
   // The user will be asked to confirm overwriting dataset or files unless
   // specified opt provides the answer!
   // If kOverwriteNoFiles is set, then a pointer to TList must be passed as
   // skippedFiles argument. The function will add to this list TFileInfo
   // objects describing all files that existed on the cluster and were
   // not uploaded.
   //

   if (fProtocol < 15) {
      Info("UploadDataSet", "functionality not available: the server has an"
                            " incompatible version of TFileInfo");
      return -1;
   }

   TList fileList;
   fileList.SetOwner();
   void *dataSetDir = gSystem->OpenDirectory(gSystem->DirName(files));
   const char* ent;
   TString filesExp(gSystem->BaseName(files));
   filesExp.ReplaceAll("*",".*");
   TRegexp rg(filesExp);
   while ((ent = gSystem->GetDirEntry(dataSetDir))) {
      TString entryString(ent);
      if (entryString.Index(rg) != kNPOS) {
         // Matching dir entry: add to the list
         TString u(Form("file://%s/%s", gSystem->DirName(files), ent));
         if (gSystem->AccessPathName(u, kReadPermission) == kFALSE)
            fileList.Add(new TFileInfo(u));
      } //if matching dir entry
   } //while
   Int_t fileCount;
   if ((fileCount = fileList.GetSize()) == 0)
      Printf("No files match your selection. The dataset will not be saved");
   else
      fileCount = UploadDataSet(dataSetName, &fileList, desiredDest,
                                opt, skippedFiles);
   return fileCount;
}

//______________________________________________________________________________
Int_t TProof::UploadDataSetFromFile(const char *dataset, const char *file,
                                    const char *dest, Int_t opt,
                                    TList *skippedFiles)
{
   // Upload files listed in "file" to PROOF cluster.
   // Where file = name of file containing list of files and
   // dataset = dataset name and opt is a combination of EUploadOpt bits.
   // Each file description (line) can include wildcards.
   // Check TFileInfo compatibility

   if (fProtocol < 15) {
      Info("UploadDataSetFromFile", "functionality not available: the server has an"
                                    " incompatible version of TFileInfo");
      return -1;
   }

   Int_t fileCount = -1;
   // Create the list to feed UploadDataSet(char *dataset, TList *l, ...)
   TList fileList;
   fileList.SetOwner();
   ifstream f;
   f.open(gSystem->ExpandPathName(file), ifstream::out);
   if (f.is_open()) {
      while (f.good()) {
         TString line;
         line.ReadToDelim(f);
         line.Strip(TString::kTrailing, '\n');
         if (gSystem->AccessPathName(line, kReadPermission) == kFALSE)
            fileList.Add(new TFileInfo(line));
      }
      f.close();
      if ((fileCount = fileList.GetSize()) == 0)
         Info("UploadDataSetFromFile",
              "no files match your selection. The dataset will not be saved");
      else
         fileCount = UploadDataSet(dataset, &fileList, dest,
                                   opt, skippedFiles);
   } else {
      Error("UploadDataSetFromFile", "unable to open the specified file");
   }
   // Done
   return fileCount;
}

//______________________________________________________________________________
Bool_t TProof::RegisterDataSet(const char *dataSetName,
                               TFileCollection *dataSet, const char* optStr)
{
   // Register the 'dataSet' on the cluster under the current
   // user, group and the given 'dataSetName'.
   // Fails if a dataset named 'dataSetName' already exists, unless 'optStr'
   // contains 'O', in which case the old dataset is overwritten.
   // If 'optStr' contains 'V' the dataset files are verified (default no
   // verification).
   // Returns kTRUE on success.

   // Check TFileInfo compatibility
   if (fProtocol < 17) {
      Info("RegisterDataSet",
           "functionality not available: the server does not have dataset support");
      return kFALSE;
   }

   if (!dataSetName || strlen(dataSetName) <= 0) {
      Info("RegisterDataSet", "specifying a dataset name is mandatory");
      return kFALSE;
   }

   TSocket *master;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("RegisterDataSet", "No connection to the master!");
      return kFALSE;
   }

   TMessage mess(kPROOF_DATASETS);
   mess << Int_t(kRegisterDataSet);
   mess << TString(dataSetName);
   mess << TString(optStr);
   mess.WriteObject(dataSet);
   Broadcast(mess);

   Bool_t result = kTRUE;
   Collect();
   if (fStatus != 0) {
      Error("RegisterDataSet", "dataset was not saved");
      result = kFALSE;
   }
   return result;
}

//______________________________________________________________________________
TMap *TProof::GetDataSets(const char *uri, const char* optStr)
{
   // Lists all datasets that match given uri.

   if (fProtocol < 15) {
      Info("GetDataSets",
           "functionality not available: the server does not have dataset support");
      return 0;
   }

   TSocket *master = 0;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("GetDataSets", "no connection to the master!");
      return 0;
   }

   TMessage mess(kPROOF_DATASETS);
   mess << Int_t(kGetDataSets);
   mess << TString(uri?uri:"");
   mess << TString(optStr?optStr:"");
   Broadcast(mess);
   Collect(kActive, fCollectTimeout);

   TMap *dataSetMap = 0;
   if (fStatus != 0) {
      Error("GetDataSets", "error receiving datasets information");
   } else {
      // Look in the list
      TMessage *retMess = (TMessage *) fRecvMessages->First();
      if (retMess && retMess->What() == kMESS_OK) {
         if (!(dataSetMap = (TMap *)(retMess->ReadObject(TMap::Class()))))
            Error("GetDataSets", "error receiving datasets");
      } else
         Error("GetDataSets", "message not found or wrong type (%p)", retMess);
   }

   return dataSetMap;
}

//______________________________________________________________________________
void TProof::ShowDataSets(const char *uri, const char* optStr)
{
   // Shows datasets in locations that match the uri.
   // By default shows the user's datasets and global ones

   if (fProtocol < 15) {
      Info("ShowDataSets",
           "functionality not available: the server does not have dataset support");
      return;
   }

   TSocket *master = 0;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("ShowDataSets",
            "no connection to the master!");
      return;
   }

   TMessage mess(kPROOF_DATASETS);
   mess << Int_t(kShowDataSets);
   mess << TString(uri?uri:"");
   mess << TString(optStr?optStr:"");
   Broadcast(mess);

   Collect(kActive, fCollectTimeout);
   if (fStatus != 0)
      Error("ShowDataSets", "error receiving datasets information");
}

//______________________________________________________________________________
TFileCollection *TProof::GetDataSet(const char *uri, const char* optStr)
{
   // Get a list of TFileInfo objects describing the files of the specified
   // dataset.

   if (fProtocol < 15) {
      Info("GetDataSet", "functionality not available: the server has an"
                         " incompatible version of TFileInfo");
      return 0;
   }

   if (!uri || strlen(uri) <= 0) {
      Info("GetDataSet", "specifying a dataset name is mandatory");
      return 0;
   }

   TSocket *master = 0;

   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("GetDataSet", "no connection to the master!");
      return 0;
   }
   TMessage nameMess(kPROOF_DATASETS);
   nameMess << Int_t(kGetDataSet);
   nameMess << TString(uri?uri:"");
   nameMess << TString(optStr?optStr:"");
   if (Broadcast(nameMess) < 0)
      Error("GetDataSet", "sending request failed");

   Collect(kActive, fCollectTimeout);
   TFileCollection *fileList = 0;
   if (fStatus != 0) {
      Error("GetDataSet", "error receiving datasets information");
   } else {
      // Look in the list
      TMessage *retMess = (TMessage *) fRecvMessages->First();
      if (retMess && retMess->What() == kMESS_OK) {
         if (!(fileList = (TFileCollection*)(retMess->ReadObject(TFileCollection::Class()))))
            Error("GetDataSet", "error reading list of files");
      } else
         Error("GetDataSet", "message not found or wrong type (%p)", retMess);
   }

   return fileList;
}

//______________________________________________________________________________
void TProof::ShowDataSet(const char *uri, const char* opt)
{
   // display meta-info for given dataset usi

   TFileCollection *fileList = 0;
   if ((fileList = GetDataSet(uri))) {
      fileList->Print(opt);
      delete fileList;
   } else
      Warning("ShowDataSet","no such dataset: %s", uri);
}

//______________________________________________________________________________
Int_t TProof::RemoveDataSet(const char *uri, const char* optStr)
{
   // Remove the specified dataset from the PROOF cluster.
   // Files are not deleted.

   TSocket *master;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("RemoveDataSet", "no connection to the master!");
      return kError;
   }
   TMessage nameMess(kPROOF_DATASETS);
   nameMess << Int_t(kRemoveDataSet);
   nameMess << TString(uri?uri:"");
   nameMess << TString(optStr?optStr:"");
   if (Broadcast(nameMess) < 0)
      Error("RemoveDataSet", "sending request failed");
   Collect(kActive, fCollectTimeout);

   if (fStatus != 0)
      return -1;
   else
      return 0;
}

//______________________________________________________________________________
TList* TProof::FindDataSets(const char* /*searchString*/, const char* /*optStr*/)
{
   // Find datasets, returns in a TList all found datasets.

   Error ("FindDataSets", "not yet implemented");
   return (TList *) 0;
}

//______________________________________________________________________________
Int_t TProof::VerifyDataSet(const char *uri, const char* optStr)
{
   // Verify if all files in the specified dataset are available.
   // Print a list and return the number of missing files.

   if (fProtocol < 15) {
      Info("VerifyDataSet", "functionality not available: the server has an"
                            " incompatible version of TFileInfo");
      return kError;
   }

   Int_t nMissingFiles = 0;
   TSocket *master;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("VerifyDataSet", "no connection to the master!");
      return kError;
   }
   TMessage nameMess(kPROOF_DATASETS);
   nameMess << Int_t(kVerifyDataSet);
   nameMess << TString(uri ? uri : "");
   nameMess << TString(optStr ? optStr : "");
   Broadcast(nameMess);

   Collect(kActive, fCollectTimeout);

   if (fStatus < 0) {
      Info("VerifyDataSet", "no such dataset %s", uri);
      return  -1;
   } else
      nMissingFiles = fStatus;
   return nMissingFiles;
}

//______________________________________________________________________________
TMap *TProof::GetDataSetQuota(const char* optStr)
{
   // returns a map of the quotas of all groups

   if (IsLite()) {
      Info("UploadDataSet", "Lite-session: functionality not implemented");
      return (TMap *)0;
   }

   TSocket *master = 0;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("GetDataSetQuota", "no connection to the master!");
      return 0;
   }

   TMessage mess(kPROOF_DATASETS);
   mess << Int_t(kGetQuota);
   mess << TString(optStr?optStr:"");
   Broadcast(mess);

   Collect(kActive, fCollectTimeout);
   TMap *groupQuotaMap = 0;
   if (fStatus < 0) {
      Info("GetDataSetQuota", "could not receive quota");
   } else {
      // Look in the list
      TMessage *retMess = (TMessage *) fRecvMessages->First();
      if (retMess && retMess->What() == kMESS_OK) {
         if (!(groupQuotaMap = (TMap*)(retMess->ReadObject(TMap::Class()))))
            Error("GetDataSetQuota", "error getting quotas");
      } else
         Error("GetDataSetQuota", "message not found or wrong type (%p)", retMess);
   }

   return groupQuotaMap;
}

//_____________________________________________________________________________
void TProof::ShowDataSetQuota(Option_t* opt)
{
   // shows the quota and usage of all groups
   // if opt contains "U" shows also distribution of usage on user-level

   if (fProtocol < 15) {
      Info("ShowDataSetQuota",
           "functionality not available: the server does not have dataset support");
      return;
   }

   if (IsLite()) {
      Info("UploadDataSet", "Lite-session: functionality not implemented");
      return;
   }

   TSocket *master = 0;
   if (fActiveSlaves->GetSize())
      master = ((TSlave*)(fActiveSlaves->First()))->GetSocket();
   else {
      Error("ShowDataSetQuota", "no connection to the master!");
      return;
   }

   TMessage mess(kPROOF_DATASETS);
   mess << Int_t(kShowQuota);
   mess << TString(opt?opt:"");
   Broadcast(mess);

   Collect();
   if (fStatus != 0)
      Error("ShowDataSetQuota", "error receiving quota information");
}

//_____________________________________________________________________________
void TProof::InterruptCurrentMonitor()
{
   // If in active in a monitor set ready state
   if (fCurrentMonitor)
      fCurrentMonitor->Interrupt();
}

//_____________________________________________________________________________
void TProof::ActivateWorker(const char *ord)
{
   // Make sure that the worker identified by the ordinal number 'ord' is
   // in the active list. The request will be forwarded to the master
   // in direct contact with the worker. If needed, this master will move
   // the worker from the inactive to the active list and rebuild the list
   // of unique workers.
   // Use ord = "*" to activate all inactive workers.

   ModifyWorkerLists(ord, kTRUE);
}

//_____________________________________________________________________________
void TProof::DeactivateWorker(const char *ord)
{
   // Remove the worker identified by the ordinal number 'ord' from the
   // the active list. The request will be forwarded to the master
   // in direct contact with the worker. If needed, this master will move
   // the worker from the active to the inactive list and rebuild the list
   // of unique workers.
   // Use ord = "*" to deactivate all active workers.

   ModifyWorkerLists(ord, kFALSE);
}

//_____________________________________________________________________________
void TProof::ModifyWorkerLists(const char *ord, Bool_t add)
{
   // Modify the worker active/inactive list by making the worker identified by
   // the ordinal number 'ord' active (add == TRUE) or inactive (add == FALSE).
   // If needed, the request will be forwarded to the master in direct contact
   // with the worker. The end-master will move the worker from one list to the
   // other active and rebuild the list of unique active workers.
   // Use ord = "*" to deactivate all active workers.

   // Make sure the input make sense
   if (!ord || strlen(ord) <= 0) {
      Info("ModifyWorkerLists",
           "An ordinal number - e.g. \"0.4\" or \"*\" for all - is required as input");
      return;
   }

   Bool_t fw = kTRUE;    // Whether to forward one step down
   Bool_t rs = kFALSE;   // Whether to rescan for unique workers

   // Appropriate list pointing
   TList *in = (add) ? fInactiveSlaves : fActiveSlaves;
   TList *out = (add) ? fActiveSlaves : fInactiveSlaves;

   if (TestBit(TProof::kIsMaster)) {
      fw = IsEndMaster() ? kFALSE : kTRUE;
      // Look for the worker in the inactive list
      if (in->GetSize() > 0) {
         TIter nxw(in);
         TSlave *wrk = 0;
         while ((wrk = (TSlave *) nxw())) {
            if (ord[0] == '*' || !strncmp(wrk->GetOrdinal(), ord, strlen(ord))) {
               // Add it to the inactive list
               if (!out->FindObject(wrk)) {
                  out->Add(wrk);
                  if (add)
                     fActiveMonitor->Add(wrk->GetSocket());
               }
               // Remove it from the active list
               in->Remove(wrk);
               if (!add) {
                  fActiveMonitor->Remove(wrk->GetSocket());
                  wrk->SetStatus(TSlave::kInactive);
               } else
                  wrk->SetStatus(TSlave::kActive);

               // Nothing to forward (ord is unique)
               fw = kFALSE;
               // Rescan for unique workers (active list modified)
               rs = kTRUE;
               // We are done, if not option 'all'
               if (ord[0] != '*')
                  break;
            }
         }
      }
   }

   // Rescan for unique workers
   if (rs)
      FindUniqueSlaves();

   // Forward the request one step down, if needed
   Int_t action = (add) ? (Int_t) kActivateWorker : (Int_t) kDeactivateWorker;
   if (fw) {
      TMessage mess(kPROOF_WORKERLISTS);
      mess << action << TString(ord);
      Broadcast(mess);
      Collect(kActive, fCollectTimeout);
   }
}

//_____________________________________________________________________________
TProof *TProof::Open(const char *cluster, const char *conffile,
                                          const char *confdir, Int_t loglevel)
{
   // Start a PROOF session on a specific cluster. If cluster is 0 (the
   // default) then the PROOF Session Viewer GUI pops up and 0 is returned.
   // If cluster is "" (empty string) then we connect to a PROOF session
   // on the localhost ("proof://localhost"). Via conffile a specific
   // PROOF config file in the confir directory can be specified.
   // Use loglevel to set the default loging level for debugging.
   // The appropriate instance of TProofMgr is created, if not
   // yet existing. The instantiated TProof object is returned.
   // Use TProof::cd() to switch between PROOF sessions.
   // For more info on PROOF see the TProof ctor.

   const char *pn = "TProof::Open";

   // Make sure libProof and dependents are loaded and TProof can be created,
   // dependents are loaded via the information in the [system].rootmap file
   if (!cluster) {

      TPluginManager *pm = gROOT->GetPluginManager();
      if (!pm) {
         ::Error(pn, "plugin manager not found");
         return 0;
      }

      if (gROOT->IsBatch()) {
         ::Error(pn, "we are in batch mode, cannot show PROOF Session Viewer");
         return 0;
      }
      // start PROOF Session Viewer
      TPluginHandler *sv = pm->FindHandler("TSessionViewer", "");
      if (!sv) {
         ::Error(pn, "no plugin found for TSessionViewer");
         return 0;
      }
      if (sv->LoadPlugin() == -1) {
         ::Error(pn, "plugin for TSessionViewer could not be loaded");
         return 0;
      }
      sv->ExecPlugin(0);
      return 0;

   } else {

      TString clst(cluster);
      if (clst.BeginsWith("workers=") || clst.BeginsWith("tunnel="))
         clst.Insert(0, "/?");

      // Parse input URL
      TUrl u(clst);

      // Parse any tunning info ("<cluster>/?tunnel=[<tunnel_host>:]tunnel_port)
      TString opts(u.GetOptions());
      if (!opts.IsNull()) {
         Int_t it = opts.Index("tunnel=");
         if (it != kNPOS) {
            TString sport = opts(it + strlen("tunnel="), opts.Length());
            TString host("127.0.0.1");
            Int_t port = -1;
            Int_t ic = sport.Index(":");
            if (ic != kNPOS) {
               // Isolate the host
               host = sport(0, ic);
               sport.Remove(0, ic + 1);
            }
            if (!sport.IsDigit()) {
               // Remove the non digit part
               TRegexp re("[^0-9]");
               Int_t ind = sport.Index(re);
               if (ind != kNPOS)
                  sport.Remove(ind);
            }
            // Set the port
            if (sport.IsDigit())
               port = sport.Atoi();
            if (port > 0) {
               // Set the relevant variables
               ::Info("TProof::Open","using tunnel at %s:%d", host.Data(), port);
               gEnv->SetValue("XNet.SOCKS4Host", host);
               gEnv->SetValue("XNet.SOCKS4Port", port);
            } else {
               // Warn parsing problems
               ::Warning("TProof::Open",
                         "problems parsing tunnelling info from options: %s", opts.Data());
            }
         }
      }

      // Find out if we are required to attach to a specific session
      Int_t locid = -1;
      Bool_t create = kFALSE;
      if (opts.Length() > 0) {
         if (opts.BeginsWith("N",TString::kIgnoreCase)) {
            create = kTRUE;
         } else if (opts.IsDigit()) {
            locid = opts.Atoi();
         }
      }

      // Attach-to or create the appropriate manager
      TProofMgr *mgr = TProofMgr::Create(u.GetUrl());

      TProof *proof = 0;
      if (mgr && mgr->IsValid()) {

         // If XProofd we always attempt an attach first (unless
         // explicitely not requested).
         Bool_t attach = (create || mgr->IsProofd() || mgr->IsLite()) ? kFALSE : kTRUE;
         if (attach) {
            TProofDesc *d = 0;
            if (locid < 0)
               // Get the list of sessions
               d = (TProofDesc *) mgr->QuerySessions("")->First();
            else
               d = (TProofDesc *) mgr->GetProofDesc(locid);
            if (d) {
               proof = (TProof*) mgr->AttachSession(d);
               if (!proof || !proof->IsValid()) {
                  if (locid)
                     ::Error(pn, "new session could not be attached");
                  SafeDelete(proof);
               }
            }
         }

         // start the PROOF session
         if (!proof) {
            proof = (TProof*) mgr->CreateSession(conffile, confdir, loglevel);
            if (!proof || !proof->IsValid()) {
               ::Error(pn, "new session could not be created");
               SafeDelete(proof);
            }
         }
      }
      return proof;
   }
}

//_____________________________________________________________________________
TProofMgr *TProof::Mgr(const char *url)
{
   // Get instance of the effective manager for 'url'
   // Return 0 on failure.

   if (!url)
      return (TProofMgr *)0;

   // Attach or create the relevant instance
   return TProofMgr::Create(url);
}

//_____________________________________________________________________________
void TProof::Reset(const char *url, Bool_t hard)
{
   // Wrapper around TProofMgr::Reset(...).

   if (url) {
      TProofMgr *mgr = TProof::Mgr(url);
      if (mgr && mgr->IsValid())
         mgr->Reset(hard);
      else
         ::Error("TProof::Reset",
                 "unable to initialize a valid manager instance");
   }
}

//_____________________________________________________________________________
const TList *TProof::GetEnvVars()
{
   // Get environemnt variables.

   return fgProofEnvList;
}

//_____________________________________________________________________________
void TProof::AddEnvVar(const char *name, const char *value)
{
   // Add an variable to the list of environment variables passed to proofserv
   // on the master and slaves

   if (gDebug > 0) ::Info("TProof::AddEnvVar","%s=%s", name, value);

   if (fgProofEnvList == 0) {
      // initialize the list if needed
      fgProofEnvList = new TList;
      fgProofEnvList->SetOwner();
   } else {
      // replace old entries with the same name
      TObject *o = fgProofEnvList->FindObject(name);
      if (o != 0) {
         fgProofEnvList->Remove(o);
      }
   }
   fgProofEnvList->Add(new TNamed(name, value));
}

//_____________________________________________________________________________
void TProof::DelEnvVar(const char *name)
{
   // Remove an variable from the list of environment variables passed to proofserv
   // on the master and slaves

   if (fgProofEnvList == 0) return;

   TObject *o = fgProofEnvList->FindObject(name);
   if (o != 0) {
      fgProofEnvList->Remove(o);
   }
}

//_____________________________________________________________________________
void TProof::ResetEnvVars()
{
   // Clear the list of environment variables passed to proofserv
   // on the master and slaves

   if (fgProofEnvList == 0) return;

   SafeDelete(fgProofEnvList);
}

//______________________________________________________________________________
void TProof::SaveWorkerInfo()
{
   // Save informations about the worker set in the file .workers in the working
   // dir. Called each time there is a change in the worker setup, e.g. by
   // TProof::MarkBad().

   // We must be masters
   if (TestBit(TProof::kIsClient))
      return;

   // We must have a server defined
   if (!gProofServ) {
      Error("SaveWorkerInfo","gProofServ undefined");
      return;
   }

   // The relevant lists must be defined
   if (!fSlaves && !fBadSlaves) {
      Warning("SaveWorkerInfo","all relevant worker lists is undefined");
      return;
   }

   // Create or truncate the file first
   TString fnwrk = Form("%s/.workers",
                        gSystem->DirName(gProofServ->GetSessionDir()));
   FILE *fwrk = fopen(fnwrk.Data(),"w");
   if (!fwrk) {
      Error("SaveWorkerInfo",
            "cannot open %s for writing (errno: %d)", fnwrk.Data(), errno);
      return;
   }

   // Loop over the list of workers (active is any worker not flagged as bad)
   TIter nxa(fSlaves);
   TSlave *wrk = 0;
   while ((wrk = (TSlave *) nxa())) {
      Int_t status = (fBadSlaves && fBadSlaves->FindObject(wrk)) ? 0 : 1;
      // Write out record for this worker
      fprintf(fwrk,"%s@%s:%d %d %s %s.log\n",
                   wrk->GetUser(), wrk->GetName(), wrk->GetPort(), status,
                   wrk->GetOrdinal(), wrk->GetWorkDir());
   }

   // Close file
   fclose(fwrk);

   // We are done
   return;
}

//______________________________________________________________________________
Int_t TProof::GetParameter(TCollection *c, const char *par, TString &value)
{
   // Get the value from the specified parameter from the specified collection.
   // Returns -1 in case of error (i.e. list is 0, parameter does not exist
   // or value type does not match), 0 otherwise.

   TObject *obj = c->FindObject(par);
   if (obj) {
      TNamed *p = dynamic_cast<TNamed*>(obj);
      if (p) {
         value = p->GetTitle();
         return 0;
      }
   }
   return -1;

}

//______________________________________________________________________________
Int_t TProof::GetParameter(TCollection *c, const char *par, Int_t &value)
{
   // Get the value from the specified parameter from the specified collection.
   // Returns -1 in case of error (i.e. list is 0, parameter does not exist
   // or value type does not match), 0 otherwise.

   TObject *obj = c->FindObject(par);
   if (obj) {
      TParameter<Int_t> *p = dynamic_cast<TParameter<Int_t>*>(obj);
      if (p) {
         value = p->GetVal();
         return 0;
      }
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::GetParameter(TCollection *c, const char *par, Long_t &value)
{
   // Get the value from the specified parameter from the specified collection.
   // Returns -1 in case of error (i.e. list is 0, parameter does not exist
   // or value type does not match), 0 otherwise.

   TObject *obj = c->FindObject(par);
   if (obj) {
      TParameter<Long_t> *p = dynamic_cast<TParameter<Long_t>*>(obj);
      if (p) {
         value = p->GetVal();
         return 0;
      }
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::GetParameter(TCollection *c, const char *par, Long64_t &value)
{
   // Get the value from the specified parameter from the specified collection.
   // Returns -1 in case of error (i.e. list is 0, parameter does not exist
   // or value type does not match), 0 otherwise.

   TObject *obj = c->FindObject(par);
   if (obj) {
      TParameter<Long64_t> *p = dynamic_cast<TParameter<Long64_t>*>(obj);
      if (p) {
         value = p->GetVal();
         return 0;
      }
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::GetParameter(TCollection *c, const char *par, Double_t &value)
{
   // Get the value from the specified parameter from the specified collection.
   // Returns -1 in case of error (i.e. list is 0, parameter does not exist
   // or value type does not match), 0 otherwise.

   TObject *obj = c->FindObject(par);
   if (obj) {
      TParameter<Double_t> *p = dynamic_cast<TParameter<Double_t>*>(obj);
      if (p) {
         value = p->GetVal();
         return 0;
      }
   }
   return -1;
}

//______________________________________________________________________________
Int_t TProof::AssertDataSet(TDSet *dset, TList *input,
                            TProofDataSetManager *mgr, TString &emsg)
{
   // Make sure that dataset is in the form to be processed. This may mean
   // retrieving the relevant info from the dataset manager or from the
   // attached input list.
   // Returns 0 on success, -1 on error

   emsg = "";

   // We must have something to process
   if (!dset || !input || !mgr) {
      emsg.Form("invalid inputs (%p, %p, %p)", dset, input, mgr);
      return -1;
   }

   TFileCollection* dataset = 0;
   TString lookupopt;
   TString dsname(dset->GetName());
   // The dataset maybe in the form of a TFileCollection in the input list
   if (dsname.BeginsWith("TFileCollection:")) {
      // Isolate the real name
      dsname.ReplaceAll("TFileCollection:", "");
      // Get the object
      dataset = (TFileCollection *) input->FindObject(dsname);
      if (!dataset) {
         emsg.Form("TFileCollection %s not found in input list", dset->GetName());
         return -1;
      }
      // Remove from everywhere
      input->RecursiveRemove(dataset);
      // Make sure we lookup everything (unless the client or the administartor
      // required something else)
      if (TProof::GetParameter(input, "PROOF_LookupOpt", lookupopt) != 0) {
         lookupopt = gEnv->GetValue("Proof.LookupOpt", "all");
         input->Add(new TNamed("PROOF_LookupOpt", lookupopt.Data()));
      }
   }

   // The received message included an empty dataset, with only the name
   // defined: assume that a dataset, stored on the PROOF master by that
   // name, should be processed.
   if (!dataset) {
      dataset = mgr->GetDataSet(dsname.Data());
      if (!dataset) {
         emsg.Form("no such dataset on the master: %s", dsname.Data());
         return -1;
      }

      // Apply the lookup option requested by the client or the administartor
      // (by default we trust the information in the dataset)
      if (TProof::GetParameter(input, "PROOF_LookupOpt", lookupopt) != 0) {
         lookupopt = gEnv->GetValue("Proof.LookupOpt", "stagedOnly");
         input->Add(new TNamed("PROOF_LookupOpt", lookupopt.Data()));
      }
   }

   // Logic for the subdir/obj names: try first to see if the dataset name contains
   // some info; if not check the settings in the TDSet object itself; if still empty
   // check the default tree name / path in the TFileCollection object; if still empty
   // use the default as the flow will determine
   TString dsTree;
   // Get the [subdir/]tree, if any
   mgr->ParseUri(dsname.Data(), 0, 0, 0, &dsTree);
   if (dsTree.IsNull()) {
      // Use what we have in the original dataset; we need this to locate the
      // meta data information
      dsTree += dset->GetDirectory();
      dsTree += dset->GetObjName();
   }
   if (!dsTree.IsNull() && dsTree != "/") {
      TString tree(dsTree);
      Int_t idx = tree.Index("/");
      if (idx != kNPOS) {
         TString dir = tree(0, idx+1);
         tree.Remove(0, idx);
         dset->SetDirectory(dir);
      }
      dset->SetObjName(tree);
   } else {
      // Use the default obj name from the TFileCollection
      dsTree = dataset->GetDefaultTreeName();
   }

   // Transfer the list now
   if (dataset) {
      TList *missingFiles = new TList;
      TSeqCollection* files = dataset->GetList();
      if (gDebug > 0) files->Print();
      Bool_t availableOnly = (lookupopt != "all") ? kTRUE : kFALSE;
      if (!dset->Add(files, dsTree, availableOnly, missingFiles)) {
         emsg.Form("error retrieving dataset %s", dset->GetName());
         delete dataset;
         return -1;
      }
      if (missingFiles) {
         // The missing files objects have to be removed from the dataset
         // before delete.
         TIter next(missingFiles);
         TObject *file;
         while ((file = next()))
            dataset->GetList()->Remove(file);
      }
      delete dataset;

      // Make sure it will be sent back merged with other similar lists created
      // during processing; this list will be transferred by the player to the
      // output list, once the latter has been created (see TProofPlayerRemote::Process)
      if (missingFiles && missingFiles->GetSize() > 0) {
         missingFiles->SetName("MissingFiles");
         input->Add(missingFiles);
      }
   }

   // Done
   return 0;
}

//______________________________________________________________________________
Int_t TProof::SaveInputData(TQueryResult *qr, const char *cachedir, TString &emsg)
{
   // Save input data file from 'cachedir' into the sandbox or create a the file
   // with input data objects

   TList *input = 0;

   // We must have got something to process
   if (!qr || !(input = qr->GetInputList()) ||
       !cachedir || strlen(cachedir) <= 0) return 0;

   // There must be some input data or input data file
   TNamed *data = (TNamed *) input->FindObject("PROOF_InputDataFile");
   TList *inputdata = (TList *) input->FindObject("PROOF_InputData");
   if (!data && !inputdata) return 0;
   // Default dstination filename
   if (!data)
      input->Add((data = new TNamed("PROOF_InputDataFile", kPROOF_InputDataFile)));

   TString dstname(data->GetTitle()), srcname;
   Bool_t fromcache = kFALSE;
   if (dstname.BeginsWith("cache:")) {
      fromcache = kTRUE;
      dstname.ReplaceAll("cache:", "");
      srcname.Form("%s/%s", cachedir, dstname.Data());
      if (gSystem->AccessPathName(srcname)) {
         emsg.Form("input data file not found in cache (%s)", srcname.Data());
         return -1;
      }
   }

   // If from cache, just move the cache file
   if (fromcache) {
      if (gSystem->CopyFile(srcname, dstname, kTRUE) != 0) {
         emsg.Form("problems copying %s to %s", srcname.Data(), dstname.Data());
         return -1;
      }
   } else {
      // Create the file
      if (inputdata && inputdata->GetSize() > 0) {
         TFile *f = TFile::Open(dstname.Data(), "RECREATE");
         if (f) {
            f->cd();
            inputdata->Write();
            f->Close();
            delete f;
         } else {
            emsg.Form("could not create %s", dstname.Data());
            return -1;
         }
      } else {
         emsg.Form("no input data!");
         return -1;
      }
   }
   ::Info("TProof::SaveInputData", "input data saved to %s", dstname.Data());

   // Save the file name and clean up the data list
   data->SetTitle(dstname);
   if (inputdata) {
      input->Remove(inputdata);
      inputdata->SetOwner();
      delete inputdata;
   }

   // Done
   return 0;
}

//______________________________________________________________________________
Int_t TProof::SendInputData(TQueryResult *qr, TProof *p, TString &emsg)
{
   // Send the input data file to the workers

   TList *input = 0;

   // We must have got something to process
   if (!qr || !(input = qr->GetInputList())) return 0;

   // There must be some input data or input data file
   TNamed *inputdata = (TNamed *) input->FindObject("PROOF_InputDataFile");
   if (!inputdata) return 0;

   TString fname(inputdata->GetTitle());
   if (gSystem->AccessPathName(fname)) {
      emsg.Form("input data file not found in sandbox (%s)", fname.Data());
      return -1;
   }

   // PROOF session must available
   if (!p || !p->IsValid()) {
      emsg.Form("TProof object undefined or invalid: protocol error!");
      return -1;
   }

   // Send to unique workers and submasters
   p->BroadcastFile(fname, TProof::kBinary, "cache");

   // Done
   return 0;
}

//______________________________________________________________________________
Int_t TProof::GetInputData(TList *input, const char *cachedir, TString &emsg)
{
   // Get the input data from the file defined in the input list

   // We must have got something to process
   if (!input || !cachedir || strlen(cachedir) <= 0) return 0;

   // There must be some input data or input data file
   TNamed *inputdata = (TNamed *) input->FindObject("PROOF_InputDataFile");
   if (!inputdata) return 0;

   TString fname;
   fname.Form("%s/%s", cachedir, inputdata->GetTitle());
   if (gSystem->AccessPathName(fname)) {
      emsg.Form("input data file not found in cache (%s)", fname.Data());
      return -1;
   }

   // Read the input data into the input list
   TFile *f = TFile::Open(fname.Data());
   if (f) {
      TList *keys = (TList *) f->GetListOfKeys();
      if (!keys) {
         emsg.Form("could not get list of object keys from file");
         return -1;
      }
      TIter nxk(keys);
      TKey *k = 0;
      while ((k = (TKey *)nxk())) {
         TObject *o = f->Get(k->GetName());
         if (o) input->Add(o);
      }
      f->Close();
      delete f;
   } else {
      emsg.Form("could not open %s", fname.Data());
      return -1;
   }

   // Done
   return 0;
}
