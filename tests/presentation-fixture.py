#!/usr/bin/env python3
"""Expand office-suite.py's native PowerPoint output for scrolling tests.

Usage: presentation-fixture.py TEMPLATE.pptx OUTPUT.pptx [SLIDES=250]
The template must be the fresh five-slide presentation made by the suite.
"""
from pathlib import Path
import re
import sys
from zipfile import ZipFile, ZIP_DEFLATED
import xml.etree.ElementTree as ET

template, destination = map(Path, sys.argv[1:3])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 250
if not 1 <= count <= 2000 or template.resolve() == destination.resolve():
    sys.exit("Use a separate output and 1–2000 slides")
p = "http://schemas.openxmlformats.org/presentationml/2006/main"
a = "http://schemas.openxmlformats.org/drawingml/2006/main"
r = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
pr = "http://schemas.openxmlformats.org/package/2006/relationships"
ct = "http://schemas.openxmlformats.org/package/2006/content-types"
with ZipFile(template) as archive:
    files = {name:archive.read(name) for name in archive.namelist()}
slide = ET.fromstring(files["ppt/slides/slide1.xml"])
assert "TuxOffice slide 1" in ''.join(slide.itertext()), "Use the suite's test template"
slide_relations = files["ppt/slides/_rels/slide1.xml.rels"]
for relation in ET.fromstring(slide_relations):
    assert relation.attrib["Type"].endswith("/slideLayout"), "Template has unexpected slide dependencies"
presentation = ET.fromstring(files["ppt/presentation.xml"])
slide_list = presentation.find(f"{{{p}}}sldIdLst")
assert slide_list is not None
slide_list.clear()
relations = ET.fromstring(files["ppt/_rels/presentation.xml.rels"])
for relation in list(relations):
    if relation.attrib["Type"].endswith("/slide"):
        relations.remove(relation)
types = ET.fromstring(files["[Content_Types].xml"])
for override in list(types):
    if re.fullmatch(r"/ppt/slides/slide\d+\.xml", override.attrib.get("PartName", "")):
        types.remove(override)
files = {name:data for name,data in files.items() if not name.startswith("ppt/slides/")}
for index in range(1, count+1):
    relationship = f"rIdPerformance{index}"
    ET.SubElement(slide_list, f"{{{p}}}sldId", {"id":str(255+index), f"{{{r}}}id":relationship})
    ET.SubElement(relations, f"{{{pr}}}Relationship", {"Id":relationship, "Type":r+"/slide", "Target":f"slides/slide{index}.xml"})
    ET.SubElement(types, f"{{{ct}}}Override", {"PartName":f"/ppt/slides/slide{index}.xml", "ContentType":"application/vnd.openxmlformats-officedocument.presentationml.slide+xml"})
    for text in slide.iter(f"{{{a}}}t"):
        if text.text and text.text.startswith(("TuxOffice slide", "Performance slide")):
            text.text = f"Performance slide {index:03d}: scrolling benchmark"
    files[f"ppt/slides/slide{index}.xml"] = ET.tostring(slide, encoding="utf-8", xml_declaration=True)
    files[f"ppt/slides/_rels/slide{index}.xml.rels"] = slide_relations
files["ppt/presentation.xml"] = ET.tostring(presentation, encoding="utf-8", xml_declaration=True)
files["ppt/_rels/presentation.xml.rels"] = ET.tostring(relations, encoding="utf-8", xml_declaration=True)
files["[Content_Types].xml"] = ET.tostring(types, encoding="utf-8", xml_declaration=True)
if "docProps/app.xml" in files:
    properties = ET.fromstring(files["docProps/app.xml"])
    for value in properties:
        if value.tag.endswith("}Slides"):
            value.text = str(count)
    files["docProps/app.xml"] = ET.tostring(properties, encoding="utf-8", xml_declaration=True)
destination.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(destination, "w", ZIP_DEFLATED) as archive:
    for name, data in files.items():
        archive.writestr(name, data)
print(f"Wrote {count} slides to {destination}")
