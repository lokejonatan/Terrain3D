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

## GPU Workflow Component Diagram

```mermaid
classDiagram
    class Terrain3D {
        +bool use_gpu_workflow
        +set_use_gpu_workflow(enabled)
    }
    
    class Terrain3DData {
        +Terrain3DGpuWorkflow* _gpu_workflow
        +bool _gpu_workflow_enabled
        +set_gpu_workflow_enabled(enabled)
        +apply_gpu_color_brush(request)
        +apply_gpu_height_brush(request)
        +process_pending_gpu_readbacks()
        +set_gpu_preview_mode(enabled)
        +finalize_gpu_preview()
    }
    
    class Terrain3DGpuWorkflow {
        +initialize(data)
        +shutdown()
        +apply_color_brush(request)
        +apply_height_brush(request)
        +set_preview_mode(enabled)
        +finalize_preview()
        +finalize_preview_blocking()
        +process_pending_readbacks(max)
        +flush_gpu_commands()
        -RegionGpuState _region_gpu_states
        -deque~PendingBrush~ _pending_brushes
        -deque~PendingBrush~ _deferred_finalizations
        -deque~BrushRequest~ _preview_brushes
    }
    
    class Terrain3DEditor {
        +operate(position, camera_dir)
        -_operate_color_gpu(regions)
        -_operate_height_gpu(regions)
    }
    
    class RegionGpuState {
        +RID color_texture
        +RID height_texture
        +Vector2i size
    }
    
    class PendingBrush {
        +MapType map_type
        +vector~BrushRegion~ regions
        +AABB edited_area
        +int64_t id
        +int pending_readbacks
    }
    
    Terrain3D --> Terrain3DData
    Terrain3DData --> Terrain3DGpuWorkflow
    Terrain3DEditor --> Terrain3DData
    Terrain3DGpuWorkflow --> RegionGpuState
    Terrain3DGpuWorkflow --> PendingBrush
```

## Brush Processing Workflow

### Standard (Non-Preview) Mode

```mermaid
sequenceDiagram
    participant Editor as Terrain3DEditor
    participant Data as Terrain3DData
    participant GPU as Terrain3DGpuWorkflow
    participant RD as RenderingDevice
    participant Region as Terrain3DRegion
    
    Editor->>Data: apply_gpu_color_brush(request)
    Data->>GPU: apply_color_brush(request)
    
    Note over GPU: Upload region textures if needed
    GPU->>GPU: _get_or_create_region_state()
    
    Note over GPU: Dispatch compute shader
    GPU->>RD: compute_list_begin()
    GPU->>RD: compute_list_dispatch()
    GPU->>RD: compute_list_end()
    
    Note over GPU: Queue readback
    GPU->>GPU: _enqueue_readback_brush()
    
    Note over Data: Process in next frame
    Data->>GPU: process_pending_readbacks(max=1)
    
    alt Async Readbacks Supported
        GPU->>RD: texture_get_data_async(callback)
        RD-->>GPU: _on_async_texture_readback()
    else Sync Fallback
        GPU->>RD: texture_get_data()
        RD-->>GPU: PackedByteArray
    end
    
    GPU->>GPU: _apply_readback_data()
    GPU->>Region: set_color_map(image)
    GPU->>GPU: _finalize_brush_readback()
    GPU->>Data: update_maps()
```

### Preview Mode Workflow

```mermaid
sequenceDiagram
    participant Editor as Terrain3DEditor
    participant Data as Terrain3DData
    participant GPU as Terrain3DGpuWorkflow
    participant RD as RenderingDevice
    participant Material as Material/Shader
    
    Note over Editor: User starts brush stroke
    Editor->>Data: set_gpu_preview_mode(true)
    Data->>GPU: set_preview_mode(true)
    
    loop Each brush application
        Editor->>Data: apply_gpu_color_brush(request)
        Data->>GPU: apply_color_brush(request)
        GPU->>RD: Dispatch compute shader
        
        Note over GPU: Immediate visual update
        GPU->>GPU: _upload_region_to_material()
        GPU->>Data: _blit_gpu_region_texture()
        Data->>Material: Update material texture
        
        Note over GPU: Defer CPU readback
        GPU->>GPU: _preview_brushes.push_back(request)
    end
    
    Note over Editor: User releases mouse
    Editor->>Data: finalize_gpu_preview()
    Data->>GPU: finalize_preview()
    
    GPU->>GPU: _coalesce_brush_queue()
    Note over GPU: Merge overlapping brushes
    
    GPU->>GPU: Move to _deferred_finalizations
    
    loop Process deferred (small batches)
        Data->>GPU: process_pending_readbacks(max=1)
        GPU->>RD: texture_get_data_async()
        RD-->>GPU: async callback
        GPU->>GPU: _apply_readback_data()
        GPU->>GPU: _finalize_brush_readback()
    end
```

## GPU State Management

```mermaid
stateDiagram-v2
    [*] --> Uninitialized
    
    Uninitialized --> Initializing: set_gpu_workflow_enabled(true)
    Initializing --> Ready: RenderingDevice available
    Initializing --> Disabled: RenderingDevice unavailable
    
    Ready --> ProcessingBrush: apply_color/height_brush()
    ProcessingBrush --> Ready: brush dispatched
    
    Ready --> PreviewMode: set_preview_mode(true)
    PreviewMode --> ProcessingPreview: apply brush
    ProcessingPreview --> PreviewMode: immediate GPU update
    PreviewMode --> Finalizing: finalize_preview()
    
    Finalizing --> DeferredProcessing: brushes coalesced
    DeferredProcessing --> ProcessingReadback: process_pending_readbacks()
    ProcessingReadback --> DeferredProcessing: more pending
    ProcessingReadback --> Ready: all complete
    
    Ready --> Shutdown: shutdown()
    PreviewMode --> Shutdown: shutdown()
    DeferredProcessing --> Shutdown: shutdown()
    Shutdown --> [*]
```

## Brush Queue Coalescing

The GPU workflow includes a brush coalescing mechanism to optimize performance by merging multiple brush operations on the same regions.

```mermaid
graph LR
    subgraph "Input Queue"
        B1[Brush 1<br/>Color Map<br/>Region A,B]
        B2[Brush 2<br/>Color Map<br/>Region B,C]
        B3[Brush 3<br/>Height Map<br/>Region A]
        B4[Brush 4<br/>Color Map<br/>Region A]
    end
    
    subgraph "Coalescing Process"
        Group1[Group by MapType]
        Merge[Merge Regions]
        Union[Union AABBs]
    end
    
    subgraph "Output Queue"
        C1[Coalesced Color<br/>Regions A,B,C<br/>Union of AABBs]
        C2[Coalesced Height<br/>Region A<br/>Single AABB]
    end
    
    B1 --> Group1
    B2 --> Group1
    B3 --> Group1
    B4 --> Group1
    
    Group1 --> Merge
    Merge --> Union
    Union --> C1
    Union --> C2
```

## Async Readback Detection

The system automatically detects whether the rendering backend supports asynchronous texture readbacks.

```mermaid
flowchart TD
    Start([Initialize GPU Workflow]) --> CreateTest[Create 1x1 test texture]
    CreateTest --> Attempt[Call texture_get_data_async]
    
    Attempt --> CheckErr{Error returned?}
    CheckErr -->|Yes| SyncMode[Use synchronous readbacks]
    CheckErr -->|No| WaitCallback[Wait for callback]
    
    WaitCallback --> NextFrame{Next frame}
    NextFrame --> CallbackFired{Callback fired?}
    
    CallbackFired -->|Yes| AsyncMode[Use async readbacks]
    CallbackFired -->|No| FallbackSync[Fallback to sync mode]
    
    AsyncMode --> Continue([Continue operations])
    SyncMode --> Continue
    FallbackSync --> Continue
```

## Data Flow: GPU Brush to Region Storage

```mermaid
flowchart TB
    subgraph "Input"
        Request[BrushRequest<br/>center, radius, strength, etc.]
    end
    
    subgraph "GPU Processing"
        Upload[Upload region height/color<br/>to GPU texture]
        Dispatch[Dispatch compute shader<br/>with push constants]
        Compute[GPU computes<br/>brush influence]
        Result[Modified GPU texture]
    end
    
    subgraph "Readback"
        Preview{Preview mode?}
        Immediate[Blit to material<br/>immediate visual update]
        Queue[Queue for later readback]
        Async[Async texture readback]
        Sync[Sync texture readback]
    end
    
    subgraph "Storage"
        Apply[Apply readback data<br/>to CPU Image]
        Update[Update Region maps]
        Material[Update material textures]
        Notify[Notify collision/instancer]
    end
    
    Request --> Upload
    Upload --> Dispatch
    Dispatch --> Compute
    Compute --> Result
    
    Result --> Preview
    Preview -->|Yes| Immediate
    Preview -->|Yes| Queue
    Preview -->|No| Async
    
    Queue --> Async
    Async --> Apply
    Sync --> Apply
    
    Apply --> Update
    Update --> Material
    Update --> Notify
```

## Key Features

### 1. Deferred CPU Readbacks (Preview Mode)
- GPU updates material textures immediately for visual feedback
- CPU readbacks deferred until brush stroke ends
- Prevents frame drops during interactive editing

### 2. Brush Coalescing
- Multiple brush strokes merged by map type
- Reduces number of GPU→CPU transfers
- Minimizes redundant processing

### 3. Async Readback Support
- Automatically detects backend capabilities
- Falls back to synchronous mode if needed
- Non-blocking texture transfers when supported

### 4. Incremental Processing
- Processes readbacks in small batches per frame
- Configurable `max_brushes` limit
- Prevents UI freezing on large edits

### 5. Region-Based GPU State
- Per-region GPU texture cache
- Textures created on-demand
- Automatic cleanup on region removal

## Performance Characteristics

| Operation | CPU Workflow | GPU Workflow (Standard) | GPU Workflow (Preview) |
|-----------|--------------|------------------------|------------------------|
| Brush dispatch | Immediate CPU processing | GPU compute dispatch | GPU compute dispatch |
| Visual update | Immediate | After readback (~1 frame) | Immediate (GPU blit) |
| CPU readback | N/A | Every brush | End of stroke (batched) |
| Frame impact | High (large brushes) | Low-Medium | Very Low |
| Best for | Small edits | General use | Interactive painting |

## Configuration

### Enable GPU Workflow
```gdscript
# In Terrain3D node
terrain.use_gpu_workflow = true
```

### Adjust Readback Batch Size
The `_gpu_readbacks_per_flush` parameter in Terrain3DData controls how many brushes are processed per frame during finalization. Lower values spread work over more frames (smoother but slower), higher values complete faster but may cause frame drops.

## Implementation Files

- **src/terrain_3d_gpu.h/cpp**: Core GPU workflow implementation
- **src/terrain_3d_data.h/cpp**: Integration with data layer
- **src/terrain_3d_editor.h/cpp**: Editor-side brush operations
- **src/shaders/terrain_gpu_brush.glsl**: Color brush compute shader
- **src/shaders/terrain_gpu_height_brush.glsl**: Height brush compute shader