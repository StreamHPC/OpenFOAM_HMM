/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2016 OpenFOAM Foundation
     \\/     M anipulation  | Copyright (C) 2017 OpenCFD Ltd.
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/
#include "vtkPVFoamReader.h"

#include "pqApplicationCore.h"
#include "pqRenderView.h"
#include "pqServerManagerModel.h"

// VTK includes
#include "vtkCallbackCommand.h"
#include "vtkDataArraySelection.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkInformationDoubleVectorKey.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkObjectFactory.h"
#include "vtkSMRenderViewProxy.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"

// OpenFOAM includes
#include "vtkPVFoam.H"

// STL includes
#include <vector>

#undef EXPERIMENTAL_TIME_CACHING

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

vtkStandardNewMacro(vtkPVFoamReader);


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

vtkPVFoamReader::vtkPVFoamReader()
{
    Debug = 0;
    vtkDebugMacro(<<"Constructor");

    SetNumberOfInputPorts(0);

    FileName = nullptr;
    backend_ = nullptr;
    output0_ = nullptr;

#ifdef VTKPVFOAM_DUALPORT
    // Add second output for the Lagrangian
    this->SetNumberOfOutputPorts(2);
    vtkMultiBlockDataSet *lagrangian = vtkMultiBlockDataSet::New();
    lagrangian->ReleaseData();

    this->GetExecutive()->SetOutputData(1, lagrangian);
    lagrangian->Delete();
#endif

    TimeStepRange[0] = 0;
    TimeStepRange[1] = 0;

    CacheMesh = true;
    Refresh = false;

    SkipZeroTime = false;
    ExtrapolatePatches = false;
    UseVTKPolyhedron = false;
    IncludeSets = false;
    IncludeZones = false;
    ShowPatchNames = false;
    ShowGroupsOnly = false;
    InterpolateVolFields = true;

    UpdateGUI = false;

    PartSelection = vtkDataArraySelection::New();
    VolFieldSelection = vtkDataArraySelection::New();
    PointFieldSelection = vtkDataArraySelection::New();
    LagrangianFieldSelection = vtkDataArraySelection::New();

    // Setup the selection callback to modify this object when an array
    // selection is changed.
    SelectionObserver = vtkCallbackCommand::New();
    SelectionObserver->SetCallback
    (
        &vtkPVFoamReader::SelectionModifiedCallback
    );
    SelectionObserver->SetClientData(this);

    PartSelection->AddObserver
    (
        vtkCommand::ModifiedEvent,
        this->SelectionObserver
    );
    VolFieldSelection->AddObserver
    (
        vtkCommand::ModifiedEvent,
        this->SelectionObserver
    );
    PointFieldSelection->AddObserver
    (
        vtkCommand::ModifiedEvent,
        this->SelectionObserver
    );
    LagrangianFieldSelection->AddObserver
    (
        vtkCommand::ModifiedEvent,
        this->SelectionObserver
    );
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

vtkPVFoamReader::~vtkPVFoamReader()
{
    vtkDebugMacro(<<"Deconstructor");

    if (backend_)
    {
        // remove patch names
        updatePatchNamesView(false);
        delete backend_;
    }

    if (FileName)
    {
        delete [] FileName;
    }

    if (output0_)
    {
        output0_->Delete();
    }


    PartSelection->RemoveAllObservers();
    VolFieldSelection->RemoveAllObservers();
    PointFieldSelection->RemoveAllObservers();
    LagrangianFieldSelection->RemoveAllObservers();

    SelectionObserver->Delete();

    PartSelection->Delete();
    VolFieldSelection->Delete();
    PointFieldSelection->Delete();
    LagrangianFieldSelection->Delete();
}


// * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * * //

// Do everything except set the output info
int vtkPVFoamReader::RequestInformation
(
    vtkInformation* vtkNotUsed(request),
    vtkInformationVector** vtkNotUsed(inputVector),
    vtkInformationVector* outputVector
)
{
    vtkDebugMacro(<<"RequestInformation");

    if (Foam::vtkPVFoam::debug)
    {
        cout<<"REQUEST_INFORMATION\n";
    }

    if (!FileName)
    {
        vtkErrorMacro("FileName has to be specified!");
        return 0;
    }

    const int nInfo = outputVector->GetNumberOfInformationObjects();

    if (Foam::vtkPVFoam::debug)
    {
        cout<<"RequestInformation with " << nInfo << " item(s)\n";
        for (int infoI = 0; infoI < nInfo; ++infoI)
        {
            outputVector->GetInformationObject(infoI)->Print(cout);
        }
    }

    if (!backend_)
    {
        backend_ = new Foam::vtkPVFoam(FileName, this);
    }
    else
    {
        backend_->updateInfo();
    }

    std::vector<double> times = backend_->findTimes(this->SkipZeroTime);

    if (times.empty())
    {
        vtkErrorMacro("could not find valid OpenFOAM mesh");

        // delete foamData and flag it as fatal error
        delete backend_;
        backend_ = nullptr;
        return 0;
    }

    // set identical time steps for all ports
    for (int infoI = 0; infoI < nInfo; ++infoI)
    {
        vtkInformation *outInfo = outputVector->GetInformationObject(infoI);

        outInfo->Set
        (
            vtkStreamingDemandDrivenPipeline::TIME_STEPS(),
            times.data(),
            static_cast<int>(times.size())
        );

        // Something like this may be useful:
        // outInfo->Set
        // (
        //     vtkStreamingDemandDrivenPipeline::TIME_DEPENDENT_INFORMATION(),
        //     1
        // );
    }

    if (times.size())
    {
        double timeRange[2]{ times.front(), times.back() };

        if (Foam::vtkPVFoam::debug > 1)
        {
            cout
                <<"nInfo " << nInfo << "\n"
                <<"time-range " << times.front() << ':' << times.back() << "\n"
                <<"times " << times.size() << "(";

            for (const double& val : times)
            {
                cout<< ' ' << val;
            }
            cout << " )" << std::endl;
        }

        for (int infoI = 0; infoI < nInfo; ++infoI)
        {
            vtkInformation *outInfo = outputVector->GetInformationObject(infoI);

            outInfo->Set
            (
                vtkStreamingDemandDrivenPipeline::TIME_RANGE(),
                timeRange,
                2
            );
        }
    }

    return 1;
}


// Set the output info
int vtkPVFoamReader::RequestData
(
    vtkInformation* vtkNotUsed(request),
    vtkInformationVector** vtkNotUsed(inputVector),
    vtkInformationVector* outputVector
)
{
    vtkDebugMacro(<<"RequestData");

    if (!FileName)
    {
        vtkErrorMacro("FileName has to be specified!");
        return 0;
    }
    if (!backend_)
    {
        // catch some previous error
        vtkErrorMacro("Reader failed - perhaps no mesh?");
        return 0;
    }

    const int nInfo = outputVector->GetNumberOfInformationObjects();

    if (Foam::vtkPVFoam::debug)
    {
        cout<<"RequestData with " << nInfo << " item(s)\n";
        for (int infoI = 0; infoI < nInfo; ++infoI)
        {
            outputVector->GetInformationObject(infoI)->Print(cout);
        }
    }

    // Get the requested time step.
    // We only support requests for a single time step

    int nRequestTime = 0;
    double requestTime[nInfo];

    // taking port0 as the lead for other outputs would be nice, but fails when
    // a filter is added - we need to check everything
    // but since PREVIOUS_UPDATE_TIME_STEPS() is protected, relay the logic
    // to the vtkPVFoam::setTime() method
    for (int infoI = 0; infoI < nInfo; ++infoI)
    {
        vtkInformation *outInfo = outputVector->GetInformationObject(infoI);

        int nsteps = outInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());

        if
        (
            outInfo->Has(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP())
         && nsteps > 0
        )
        {
            requestTime[nRequestTime] =
            (
                1 == nsteps
                // Only one time-step available, UPDATE_TIME_STEP is unreliable
              ? outInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS(), 0)
              : outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_TIME_STEP())
            );

            // outInfo->Set(vtkDataObject::DATA_TIME_STEP(), requestTime[nRequestTime]);
            // this->SetTimeValue(requestedTimeValue);
            ++nRequestTime;
        }
    }

    if (nRequestTime)
    {
        backend_->setTime(nRequestTime, requestTime);
    }

    vtkMultiBlockDataSet* output = vtkMultiBlockDataSet::SafeDownCast
    (
        outputVector->GetInformationObject(0)->Get
        (
            vtkMultiBlockDataSet::DATA_OBJECT()
        )
    );

    if (Foam::vtkPVFoam::debug)
    {
        cout<< "update output with "
            << output->GetNumberOfBlocks() << " blocks\n";
    }


#ifdef EXPERIMENTAL_TIME_CACHING
    bool needsUpdate = false;

    if (!output0_)
    {
        output0_ = vtkMultiBlockDataSet::New();
        needsUpdate = true;
    }

    // This experimental bit of code seems to work for the geometry,
    // but trashes the fields and still triggers the GeometryFilter
    if (needsUpdate)
    {
        backend_->Update(output);
        output0_->ShallowCopy(output);
    }
    else
    {
        output->ShallowCopy(output0_);
    }

    if (Foam::vtkPVFoam::debug)
    {
        if (needsUpdate)
        {
            cout<< "full UPDATE ---------\n";
        }
        else
        {
            cout<< "cached UPDATE ---------\n";
        }

        cout<< "UPDATED output: ";
        output->Print(cout);

        cout<< "UPDATED output0_: ";
        output0_->Print(cout);
    }

#else

#ifdef VTKPVFOAM_DUALPORT
    backend_->Update
    (
        output,
        vtkMultiBlockDataSet::SafeDownCast
        (
            outputVector->GetInformationObject(1)->Get
            (
                vtkMultiBlockDataSet::DATA_OBJECT()
            )
        );
    );
#else
    backend_->Update(output, output);
#endif

    updatePatchNamesView(ShowPatchNames);

#endif

    // Do any cleanup on the OpenFOAM side
    backend_->CleanUp();

    return 1;
}


void vtkPVFoamReader::PrintInfo()
{
    if (backend_)
    {
        backend_->printInfo();
    }
    else
    {
        cout
            <<"OpenFOAM reader not initialized\n"
            << std::flush;
    }
}


void vtkPVFoamReader::SetRefresh(bool val)
{
    Modified();
}


void vtkPVFoamReader::SetIncludeSets(bool val)
{
    if (IncludeSets != val)
    {
        IncludeSets = val;
        if (backend_)
        {
            backend_->updateInfo();
        }
    }
}


void vtkPVFoamReader::SetIncludeZones(bool val)
{
    if (IncludeZones != val)
    {
        IncludeZones = val;
        if (backend_)
        {
            backend_->updateInfo();
        }
    }
}


void vtkPVFoamReader::SetShowPatchNames(bool val)
{
    if (ShowPatchNames != val)
    {
        ShowPatchNames = val;
        updatePatchNamesView(ShowPatchNames);
    }
}


void vtkPVFoamReader::SetShowGroupsOnly(bool val)
{
    if (ShowGroupsOnly != val)
    {
        ShowGroupsOnly = val;
        if (backend_)
        {
            backend_->updateInfo();
        }
    }
}


void vtkPVFoamReader::updatePatchNamesView(const bool show)
{
    pqApplicationCore* appCore = pqApplicationCore::instance();

    // need to check this, since our destructor calls this
    if (!appCore)
    {
        return;
    }

    // Server manager model for querying items in the server manager
    pqServerManagerModel* smModel = appCore->getServerManagerModel();

    if (!smModel || !backend_)
    {
        return;
    }

    // Get all the pqRenderView instances
    QList<pqRenderView*> renderViews = smModel->findItems<pqRenderView*>();

    for (int viewI=0; viewI < renderViews.size(); ++viewI)
    {
        backend_->renderPatchNames
        (
            renderViews[viewI]->getRenderViewProxy()->GetRenderer(),
            show
        );
    }

    // Use refresh here?
}


void vtkPVFoamReader::PrintSelf(ostream& os, vtkIndent indent)
{
    vtkDebugMacro(<<"PrintSelf");

    this->Superclass::PrintSelf(os,indent);
    os  << indent << "File name: "
        << (this->FileName ? this->FileName : "(none)") << "\n";

    backend_->PrintSelf(os, indent);

    os  << indent << "Time step range: "
        << this->TimeStepRange[0] << " - " << this->TimeStepRange[1] << "\n"
        << indent << "Time step: " << this->GetTimeStep() << endl;
}


int vtkPVFoamReader::GetTimeStep()
{
    return backend_ ? backend_->timeIndex() : -1;
}


// ----------------------------------------------------------------------
// Parts selection list control

vtkDataArraySelection* vtkPVFoamReader::GetPartSelection()
{
    return PartSelection;
}

int vtkPVFoamReader::GetNumberOfPartArrays()
{
    return PartSelection->GetNumberOfArrays();
}

const char* vtkPVFoamReader::GetPartArrayName(int index)
{
    return PartSelection->GetArrayName(index);
}

int vtkPVFoamReader::GetPartArrayStatus(const char* name)
{
    return PartSelection->ArrayIsEnabled(name);
}

void vtkPVFoamReader::SetPartArrayStatus(const char* name, int status)
{
    vtkDebugMacro("Set mesh part \"" << name << "\" status to: " << status);

    if (status)
    {
        PartSelection->EnableArray(name);
    }
    else
    {
        PartSelection->DisableArray(name);
    }
}


// ----------------------------------------------------------------------
// volField selection list control

vtkDataArraySelection* vtkPVFoamReader::GetVolFieldSelection()
{
    return VolFieldSelection;
}

int vtkPVFoamReader::GetNumberOfVolFieldArrays()
{
    return VolFieldSelection->GetNumberOfArrays();
}

const char* vtkPVFoamReader::GetVolFieldArrayName(int index)
{
    return VolFieldSelection->GetArrayName(index);
}

int vtkPVFoamReader::GetVolFieldArrayStatus(const char* name)
{
    return VolFieldSelection->ArrayIsEnabled(name);
}

void vtkPVFoamReader::SetVolFieldArrayStatus(const char* name, int status)
{
    if (status)
    {
        VolFieldSelection->EnableArray(name);
    }
    else
    {
        VolFieldSelection->DisableArray(name);
    }
}


// ----------------------------------------------------------------------
// pointField selection list control

vtkDataArraySelection* vtkPVFoamReader::GetPointFieldSelection()
{
    return PointFieldSelection;
}

int vtkPVFoamReader::GetNumberOfPointFieldArrays()
{
    return PointFieldSelection->GetNumberOfArrays();
}

const char* vtkPVFoamReader::GetPointFieldArrayName(int index)
{
    return PointFieldSelection->GetArrayName(index);
}

int vtkPVFoamReader::GetPointFieldArrayStatus(const char* name)
{
    return PointFieldSelection->ArrayIsEnabled(name);
}

void vtkPVFoamReader::SetPointFieldArrayStatus(const char* name, int status)
{
    if (status)
    {
        PointFieldSelection->EnableArray(name);
    }
    else
    {
        PointFieldSelection->DisableArray(name);
    }
}


// ----------------------------------------------------------------------
// lagrangianField selection list control

vtkDataArraySelection* vtkPVFoamReader::GetLagrangianFieldSelection()
{
    return LagrangianFieldSelection;
}

int vtkPVFoamReader::GetNumberOfLagrangianFieldArrays()
{
    return LagrangianFieldSelection->GetNumberOfArrays();
}

const char* vtkPVFoamReader::GetLagrangianFieldArrayName(int index)
{
    return LagrangianFieldSelection->GetArrayName(index);
}

int vtkPVFoamReader::GetLagrangianFieldArrayStatus(const char* name)
{
    return LagrangianFieldSelection->ArrayIsEnabled(name);
}

void vtkPVFoamReader::SetLagrangianFieldArrayStatus
(
    const char* name,
    int status
)
{
    if (status)
    {
        LagrangianFieldSelection->EnableArray(name);
    }
    else
    {
        LagrangianFieldSelection->DisableArray(name);
    }
}


// ----------------------------------------------------------------------

void vtkPVFoamReader::SelectionModifiedCallback
(
    vtkObject*,
    unsigned long,
    void* clientdata,
    void*
)
{
    static_cast<vtkPVFoamReader*>(clientdata)->SelectionModified();
}


void vtkPVFoamReader::SelectionModified()
{
    vtkDebugMacro(<<"SelectionModified");
    Modified();
}


int vtkPVFoamReader::FillOutputPortInformation
(
    int port,
    vtkInformation* info
)
{
    if (port == 0)
    {
        return this->Superclass::FillOutputPortInformation(port, info);
    }
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkMultiBlockDataSet");
    return 1;
}


// ************************************************************************* //
