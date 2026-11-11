# Custom Themes

The interface can be restyled with your own themes, without rebuilding the emulator.

## Installing a theme

1. Open **Emulation > Configure > General > UI** and press **Open Themes Folder**.
   This is the `themes` folder of the user folder.
2. Put the theme's folder there. A theme is a folder that holds a `style.qss`:

   ```
   themes/
     sage-dark/
       style.qss
       theme.ini      (optional)
       icons/         (optional, whatever the stylesheet uses)
   ```

3. Open the configuration again and pick the theme in the **Theme** list, below the built-in ones.

An example theme is in [`dist/custom_themes/sage-dark`](../../dist/custom_themes/sage-dark).

## Writing a theme

`style.qss` is a [Qt style sheet](https://doc.qt.io/qt-6/stylesheet-reference.html), close to CSS.
Saving it, or `theme.ini`, updates the running emulator: no restart.

Files of the theme's folder are reached with the `theme:` prefix:

```css
QCheckBox::indicator:checked {
    image: url("theme:icons/check.svg");
}
```

### theme.ini

Every key is optional.

```ini
[Theme]
name=Sage Dark
base=lichii
dark=true
inherit=false
```

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | the folder's name | The name in the theme list. |
| `base` | `lichii` | The built-in theme that provides the icons and the palette: `default`, `colorful`, `qdarkstyle`, `colorful_dark`, `qdarkstyle_midnight_blue`, `colorful_midnight_blue` or `lichii`. |
| `dark` | whether `base` is dark | Dark title bar and link colours. |
| `inherit` | `false` | `true` applies the base theme's stylesheet first, then `style.qss`. Use it for a theme that changes a few things of a built-in one. |

### Performance overlay

The overlay paints itself, so its colours are properties of `PerformanceOverlay`:

```css
PerformanceOverlay {
    qproperty-backgroundColor: rgba(21, 24, 22, 230);
    qproperty-borderColor: #303531;
    qproperty-cornerRadius: 10;
    qproperty-accentColor: #74b384;
}
```

| Property | Used for |
| --- | --- |
| `backgroundColor` | The panel. |
| `borderColor` | The panel's outline; none by default. |
| `cornerRadius` | The panel's rounded corners, in pixels. |
| `textColor` | Values. |
| `mutedColor` | Units, tags and secondary values. |
| `dimColor` | The faintest labels. |
| `accentColor` | Headings, the limiting load and the frame rate chart's line. |
| `lineColor` | The frame time graph's line. |
| `spikeColor` | Frame time spikes. |
| `axisColor` | Axes and grid lines. |
| `medianLineColor` | The frame time graph's median line. |
| `graphBackgroundColor` | The frame rate chart's background (full style). |

A property the stylesheet doesn't set keeps its built-in colour.

## Performance overlay position and style

**Emulation > Configure > General > UI** also sets:

- **Performance overlay position**: the corner of the window it sits in. It can still be dragged;
  choosing another corner or style puts it back in its corner.
- **Performance overlay style**:
  - *Minimal*: frame rate and frame time.
  - *Standard*: frame rate, frame time, CPU and GPU load, and the frame time graph.
  - *Full*: frame time and frame rate with their minimum, maximum and average, CPU and GPU load,
    and the frame rate chart of the last minute.
