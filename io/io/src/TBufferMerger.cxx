// @(#)root/io:$Id$
// Author: Philippe Canal, Witold Pokorski, and Guilherme Amadio

/*************************************************************************
 * Copyright (C) 1995-2017, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "ROOT/TBufferMerger.hxx"

#include "TBufferFile.h"
#include "TError.h"
#include "TFileMerger.h"
#include "TROOT.h"
#include "TVirtualMutex.h"

namespace ROOT {
namespace Experimental {

TBufferMerger::TBufferMerger(const char *name, Option_t *option, Int_t compress)
{
   // NOTE: We cannot use ctor chaining or in-place initialization because we want this operation to have no effect on
   // ROOT's gDirectory.
   TDirectory::TContext ctxt;
   if (TFile *output = TFile::Open(name, option, /*title*/ name, compress))
      Init(std::unique_ptr<TFile>(output));
   else
      Error("OutputFile", "cannot open the MERGER output file %s", name);
}

TBufferMerger::TBufferMerger(std::unique_ptr<TFile> output)
{
   Init(std::move(output));
}

void TBufferMerger::Init(std::unique_ptr<TFile> output)
{
   fFile = output.release();
   fAutoSave = 0;
   RegisterCallback(nullptr);
   fMergingThread.reset(new std::thread([&]() { this->WriteOutputFile(); }));
}

TBufferMerger::~TBufferMerger()
{
   for (auto f : fAttachedFiles)
      if (!f.expired()) Fatal("TBufferMerger", " TBufferMergerFiles must be destroyed before the server");

   this->Push(nullptr);
   fMergingThread->join();
}

std::shared_ptr<TBufferMergerFile> TBufferMerger::GetFile()
{
   R__LOCKGUARD(gROOTMutex);
   std::shared_ptr<TBufferMergerFile> f(new TBufferMergerFile(*this));
   gROOT->GetListOfFiles()->Remove(f.get());
   fAttachedFiles.push_back(f);
   return f;
}

size_t TBufferMerger::GetQueueSize() const
{
   return fQueue.size();
}

void TBufferMerger::RegisterCallback(const std::function<void(std::function<void()>)> &f)
{
  if (nullptr == f) { 
     fCallback = [](const std::function<void()> &f){ f(); };
  } else {
     fCallback = f;
  }
}

void TBufferMerger::Push(TBufferFile *buffer)
{
   {
      std::lock_guard<std::mutex> lock(fQueueMutex);
      fQueue.push(buffer);
   }
   fDataAvailable.notify_one();
}

size_t TBufferMerger::GetAutoSave() const
{
   return fAutoSave;
}

void TBufferMerger::SetAutoSave(size_t size)
{
   fAutoSave = size;
}

void TBufferMerger::WriteOutputFile()
{
   size_t buffered = 0;
   TFileMerger merger;
   std::unique_ptr<TBufferFile> buffer;
   std::vector<std::unique_ptr<TMemFile>> memfiles;

   merger.ResetBit(kMustCleanup);

   {
      R__LOCKGUARD(gROOTMutex);
      merger.OutputFile(std::unique_ptr<TFile>(fFile));
   }

   auto mergefn = [&merger] { merger.PartialMerge(); merger.Reset(); };

   while (true) {
      std::unique_lock<std::mutex> lock(fQueueMutex);
      fDataAvailable.wait(lock, [this]() { return !this->fQueue.empty(); });

      buffer.reset(fQueue.front());
      fQueue.pop();
      lock.unlock();

      if (!buffer)
         break;

      Long64_t length;
      buffer->SetReadMode();
      buffer->SetBufferOffset();
      buffer->ReadLong64(length);
      buffered += length;

      memfiles.emplace_back(new TMemFile(fFile->GetName(), buffer->Buffer() + buffer->Length(), length, "read"));
      merger.AddFile(memfiles.back().get(), false);

      if (buffered > fAutoSave) {
         buffered = 0;
         fCallback(mergefn);
         memfiles.clear();
      }
   }

   auto finalmergefn = [&merger] {
      R__LOCKGUARD(gROOTMutex);
      merger.PartialMerge();
      merger.Reset();
   };
   fCallback(finalmergefn);
}

} // namespace Experimental
} // namespace ROOT
