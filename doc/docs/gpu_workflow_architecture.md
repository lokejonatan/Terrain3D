# GPU Workflow Architecture

This document describes the GPU-accelerated brush workflow architecture introduced to optimize terrain editing performance.

## Overview

The GPU workflow moves terrain brush operations from CPU to GPU, enabling:
- Real-time preview of brush strokes without CPU readbacks
- Asynchronous GPU→CPU data transfer
- Batch processing of brush strokes to reduce overhead
- Improved performance for large terrain edits

## System Architecture with GPU Workflow Changes

This diagram shows the complete Terrain3D architecture with the **new GPU workflow components highlighted in green**. Components and connections shown in standard colors were part of the original architecture.

```mermaid
graph TD
    %% Styling
    classDef main fill:#00558C,stroke:#333,stroke-width:2px,color:#fff;
    classDef component fill:#444,stroke:#333,stroke-width:1px,color:#fff;
    classDef plugin fill:#4B0082,stroke:#333,stroke-width:1px,color:#fff;
    classDef gpu fill:#4CAF50,stroke:#333,stroke-width:1px,color:#fff;
    classDef gpuNew fill:#2E7D32,stroke:#1B5E20,stroke-width:3px,color:#fff;
    classDef resource fill:#666,stroke:#333,stroke-width:1px,color:#fff;

    %% Main Nodes
    T3D[Terrain3D<br/>* Mesh generation<br/>* Collision generation<br/>* Camera snapping<br/>* GPU workflow toggle]:::main
    
    T3DM[Terrain3DMaterial<br/>* Saveable resource<br/>* Combines shader snippets<br/>* Exposes custom shader]:::resource
    
    GPU_HW[GPU Hardware]:::gpu

    T3DI[Terrain3DInstancer<br/>* Manages MMIs]:::component
    
    T3DD[Terrain3DData<br/>* Manages region data<br/>* Creates TextureArrays f/ maps<br/>* GPU workflow integration]:::component
    
    T3DA[Terrain3DAssets<br/>* List of assets<br/>* Creates TextureArrays f/ textures]:::component

    GCM[GeoClipMap<br/>* Creates mesh components]:::component
    
    T3DR[Terrain3DRegion<br/>* Stores height, control, color<br/>maps, instances]:::component
    
    GT[GeneratedTexture<br/>* Creates TextureArrays<br/>in RenderingServer]:::component

    T3DTA[Terrain3DTextureAsset<br/>* Albedo + Height tex<br/>* Normal + Rough tex<br/>* Texture settings]:::component
    
    T3DMA[Terrain3DMeshAsset<br/>* Scene File<br/>* Mesh settings]:::component

    T3DE[Terrain3DEditor<br/>* C++ editing functions<br/>* Operates on Data & Instancer<br/>* Undo, redo<br/>* GPU brush operations]:::component
    
    EP[EditorPlugin<br/>* GDScript EditorPlugin<br/>* Interacts w/ C++<br/>* Manages UI]:::plugin

    %% NEW GPU Workflow Components
    T3DGPU[Terrain3DGpuWorkflow<br/>* GPU brush processing<br/>* Async readback management<br/>* Preview mode<br/>* Brush queue coalescing]:::gpuNew
    
    ColorPipeline[GPU Color Pipeline<br/>* Compute shader<br/>* Brush rendering]:::gpuNew
    
    HeightPipeline[GPU Height Pipeline<br/>* Compute shader<br/>* Height modification]:::gpuNew

    %% Utilities
    Const[Constants<br/>* Constants<br/>* Syntactic sugar]:::component
    Log[Logger<br/>* Logging macro filters<br/>messages by log level]:::component
    Util[Terrain3DUtil<br/>* Useful static functions]:::component

    %% Original Connections
    T3D --> T3DM
    T3DM -.-> GPU_HW
    
    T3D --> T3DI
    T3D --> T3DD
    T3D --> T3DA
    T3D --> GCM

    T3DD --> GCM
    T3DD --> T3DR
    T3DD --> GT
    
    T3DA --> GT
    T3DA --> T3DTA
    T3DA --> T3DMA

    EP --> T3DE
    T3DE --> T3DD
    T3DE --> T3DI

    %% NEW GPU Workflow Connections (highlighted with thick arrows)
    T3D ==>|enable/disable| T3DGPU
    T3DD ==>|manages| T3DGPU
    T3DE ==>|apply_gpu_brush| T3DGPU
    T3DGPU ==>|uses| ColorPipeline
    T3DGPU ==>|uses| HeightPipeline
    ColorPipeline -.->|executes on| GPU_HW
    HeightPipeline -.->|executes on| GPU_HW
    T3DGPU ==>|readback to| T3DR
    T3DGPU -.->|updates| T3DM
```

**Key Changes:**
- **Terrain3DGpuWorkflow** (new): Core GPU workflow manager that handles brush processing on GPU
- **GPU Pipelines** (new): Compute shader pipelines for color and height brush operations
- **Terrain3DData integration**: Now manages GPU workflow lifecycle
- **Terrain3DEditor integration**: Calls GPU brush operations when workflow is enabled
- **Direct GPU execution**: Brush operations execute as compute shaders on GPU hardware
- **Async readback**: Modified textures are read back to CPU asynchronously

## Detailed System Architecture

```mermaid
graph TB
    subgraph "Editor Layer"
        Editor[Terrain3DEditor]
    end
    
    subgraph "Data Layer"
        Data[Terrain3DData]
        GPU[Terrain3DGpuWorkflow]
    end
    
    subgraph "GPU Resources"
        ColorPipeline[Color Brush Pipeline]
        HeightPipeline[Height Brush Pipeline]
        Shaders[Compute Shaders]
        Textures[GPU Textures]
    end
    
    subgraph "Storage"
        Regions[Terrain3DRegion]
        Images[Image Data]
    end
    
    Editor -->|apply_gpu_color_brush| Data
    Editor -->|apply_gpu_height_brush| Data
    Data -->|delegates to| GPU
    GPU -->|uses| ColorPipeline
    GPU -->|uses| HeightPipeline
    ColorPipeline --> Shaders
    HeightPipeline --> Shaders
    Shaders -->|modifies| Textures
    GPU -->|readback| Images
    Images -->|stored in| Regions
    Data -->|manages| Regions
```