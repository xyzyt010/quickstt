# QuickSTT — Comprehensive UI/UX, Multi-Node, and Clipboard-Sync Remediation Specification

## 1. Purpose, Scope, and How to Read This Document

This is the organized, authoritative specification derived from the user's raw feedback about the QuickSTT desktop application. Its purpose is threefold: preserve every piece of context, complaint, requirement, and instruction without removing or losing any of it; reorganize the material into a professional document in which every task is enumerated; and serve as an executable checklist and acceptance contract. Every line maps back to something the user actually said or to an operating rule governing the work.

Section 2 reproduces the feedback verbatim so no context is lost through paraphrase. Section 3 sets the governing principles. Sections 4 through 9 expand each problem area into actionable requirements. Section 10 defines verification and the mandatory loops. Section 11 lists every task so none can be skipped. Sections 12 and 13 state acceptance criteria, constraints, and roles.

Emphatic feedback is preserved as hard requirements, not emotional noise. Phrases such as "whole another nightmare," "it is a facade and fake," "not properly aligned neither is it proper at all nor is it properly sized," and "do not stop until you are 99 percent sure" become concrete, testable acceptance conditions. The demands that the document be "more organised," "under 4k words," and that "each and every task must be listed" are satisfied by Sections 11 and 12 and the verification loops.

## 2. Verbatim Source Text (Preserved Without Alteration)

> the ui overall is really bad 1.settings section the text in the allowed connections in each box again i have to say this the computer name should be on the left side starting with it and then a horizontal size of perfect appropriate length and thickness in between them perfectly smooth minimal but proper looking (-) and then the ip address and then on the right edge of the box the delete button and then besides it a 3 dot buttons showing more details about options for details about that device like device keys and anything else same minimal way also the trash can icon is not properly aligned neither is it proper at all nor is it properly sized and also before the name on the left edge add a indexing number 1. 2. like that also why is the plus icon and the text so weirdly palced there at the top with no proper alignment at all fix that too perfectly 2.devices section is whole another nightmare it has so many misalignments overflow unnecessary text and completely dysfunctional section nothing there is working inspect each components each section and everything and compare with proper modern ui ux and fix it all backend problems in the gui as well as simple frontend make sure to open the multi node grid and fix problems for that to do a full vision loops super iteratively make sure to not stop until you are 99 percent sure this looks modern cohesive properly sized and perfect 3. connect section overlay good mapping but maybe the text is a little too small and not everything is cohesively aligned fuully like this computer and the connected computer green text should be perfect aligned on top of each other should start at the same line the dot and the this pc words and lastly it should show all the pc listed one by one righr now it shows only one as i am only connected to one but it will show android there too seperately as yellow dots and the other computers in multi node setup also multi node setup does nto really work properly at all it is a facade and fake inspect it perfectly and indepth and find probelms if any make sure it is a read to go fully usable protocol from backend it should be perfectly professional and working do not verify with backend but do an iterative loops of testing code and criticising it with a criticising midnset and work towards until the code is perfect for the multi node setup perfectly 4.overall there is not proper cohesiveness in theme and generally like the text in settings is perfectly big and good but in other sections it is super small fix it perfectly too. and other things like that . now for other problems clipboard syncing still does not work perfectly do you understand that clipboard syncing securely perfectly at lower level needs to happen perfectly for text and files under the specified limit in mb and the text clipboard syncing needs to happen in a way that whatever i copied last on any one of the computer needs to be copied into the other so the last copied thing on either machine should be saved as path in the other computer for file if the file is under the specified limit it can be transferred via local sharing fast and efficient connection based data sharing for copying and pasting file from computer 1 and into computer 2 if the file is to big it will just not sync the copying and show a notification the file size is too big for copying in thekvm a smart disappearing temporary notification on the machine that i performing that copy. verify all of this except the clipboard mechanism with vision on mint in background verify all the gui stuff and do it in physicaly use and code so black box +white box for all the gui and ui ux stuff and autonomously make suggestions and autonomously evaluate and fix remove or add them with vision and all of that and lastly for backend features let the user test it end to end but for subtle things you can check the if the clipboard is syncing across both machines perfectly make sure to go beyond just the first copying because make sure when they both get connected make sure the clipboard is not synced it only syncs after they are connected and then any one machine copies something

## 3. Operating Principles and Verification Philosophy

Five principles govern the work. First, preserve context: no complaint, however informally phrased, may be discarded. Second, organize everything: every requirement gets a logical section and a unique task identifier. Third, verify iteratively: nothing is complete until it survives repeated, skeptical loops of testing and criticism. Fourth, verify in the correct environment: Windows is launch-only; all behavioral verification happens on the Mint box over SSH. Fifth, separate roles: the agent verifies GUI/UI/UX autonomously with vision and code, the user performs end-to-end backend testing, and the agent checks subtle clipboard timing as the sole exception.

"Do not verify with backend" applies specifically to multi-node protocol work: validate through iterative code testing and critical self-review rather than claiming backend correctness. "Let the user test it end to end" applies to backend features generally. The scopes are distinct: multi-node code correctness comes from agent code loops; end-to-end product validation belongs to the user.

The user explicitly authorizes autonomous suggestions, evaluation, and fixes, including removing or adding items, supported by vision. The agent must be its own critic: inspect, list problems, propose fixes, apply them, then re-criticize skeptically until the confidence threshold is met.

## 4. Settings Section — Allowed Connections Entries

The Settings "Allowed connections" area is the first problem area. The user says the text in each box is wrong and restates it emphatically ("again i have to say this"), signaling prior attempts failed. The required layout, left to right, is strictly defined.

First, at the far left edge, an indexing number must appear: "1.", "2.", and so on, one per entry. This is new and must be added. Second, immediately after the index, the computer name appears, starting from the left side; it is the leading textual element, preceded only by the index. Third, between name and IP there must be a horizontal separator: a dash of perfect, appropriate length and thickness, perfectly smooth, minimal, and proper-looking — not crude or oversized, but a refined typographic separator. Fourth, the IP address follows the dash. The row is therefore: index, name, smooth minimal dash, IP.

Fifth, on the right edge there must be a delete button. Sixth, beside it a three-dot button showing more details and options for that device, including device keys and anything else relevant, in the same minimal style. Because the user stresses "same minimal way," the overflow menu must not introduce a heavier visual language.

Seventh, the trash can icon is rejected on three independent grounds: not properly aligned, not proper at all, not properly sized. All three must be corrected and verified separately. The delete affordance must sit exactly on the right edge on every row without vertical or horizontal drift relative to the text baseline.

Eighth, the plus icon and its text at the top are "weirdly palced" with "no proper alignment at all" and must be fixed perfectly. The add control and label must form one coherent, aligned header element with deliberate spacing, consistent with the minimal language inside the boxes.

## 5. Devices Section — Full Audit and Repair

Devices is "whole another nightmare": "so many misalignments overflow unnecessary text and completely dysfunctional section nothing there is working." These are distinct defect classes — alignment, overflow, unnecessary text, functional failure — and the section must be treated as entirely dysfunctional until proven otherwise.

The process is explicit: inspect each component, each section, and everything; compare against proper modern UI/UX; fix it all. This is a component-by-component audit, not a cosmetic pass: every element is catalogued, compared to a modern baseline, and corrected or removed. "Unnecessary text" must be deleted, not restyled.

Both layers must be repaired: "backend problems in the gui" and "simple frontend" problems. Backend problems manifesting in the GUI must be traced to their source, not masked; frontend problems fixed simply and directly. "Make sure to open the multi node grid and fix problems for that too" requires opening and inspecting the grid as part of this pass.

Completion requires "full vision loops super iteratively" and "do not stop until you are 99 percent sure this looks modern cohesive properly sized and perfect." Modern, cohesive, properly sized, and perfect are four independent conditions, each confirmed through repeated visual loops. The 99 percent threshold means iterating past the first plausible-looking result.

## 6. Multi-Node Grid and the Multi-Node Protocol

Multi-node is the most serious complaint: it "does nto really work properly at all" and "it is a facade and fake" — a claim of non-functionality, not minor visual defects. The response is to "inspect it perfectly and indepth and find probelms if any" and "make sure it is a read to go fully usable protocol from backend it should be perfectly professional and working."

Two deliverables follow: a truthful in-depth inspection enumerating real problems, and a backend protocol genuinely ready to go, fully usable, professional, and working. A facade is unacceptable: UI displaying nodes without a functioning protocol fails.

The verification instruction must be followed exactly: "do not verify with backend but do an iterative loops of testing code and criticising it with a criticising midnset and work towards until the code is perfect for the multi node setup perfectly." The agent must not close this by asserting the backend is fine. It must repeatedly test and criticize the code with a deliberately skeptical mindset. The loops are the evidence: each produces criticisms, each criticism is resolved or explicitly justified, and iteration continues until none remain.

Devices requires opening the grid and multi-node requires a working protocol, so the two are coupled: visual correctness and protocol correctness must both hold.

## 7. Connect Section — Overlay, Mapping, and Alignment

Connect receives qualified praise and specific corrections. The overlay mapping is "good," so it must be preserved, not redesigned. The text is "a little too small" and must be increased to a readable scale. "Not everything is cohesively aligned fuully" means full cohesive alignment is required, not partial.

A precise defect is named: "this computer and the connected computer green text should be perfect aligned on top of each other should start at the same line the dot and the this pc words." The status dot and the words "this pc" must begin on the same line, with the connected computer's green text perfectly aligned on top. This is a pixel-level alignment requirement, verified visually.

The final requirement is list completeness: "it should show all the pc listed one by one righr now it shows only one as i am only connected to one." Only the active PC currently shows; the fix lists all PCs one by one. Android devices appear "seperately as yellow dots," distinct from computers, and other multi-node computers must also be listed. The overlay must reflect the real node set, not a single-connection fiction.

## 8. Overall Theme and Typography Cohesiveness

"Overall there is not proper cohesiveness in theme" makes cohesion a global requirement. A concrete instance: "generally like the text in settings is perfectly big and good but in other sections it is super small fix it perfectly too." Settings text is the reference standard; typography must be consistent across Settings, Devices, Connect, and the multi-node grid, with smaller text elsewhere treated as a defect.

"and other things like that" is a catch-all that must be honored. It authorizes the agent to identify and autonomously fix similar cohesion defects: a systematic sweep for inconsistent font sizes, spacing, alignment, and icon treatment across all sections, with each inconsistency logged and corrected.

## 9. Clipboard Syncing — Secure, Low-Level, Last-Write-Wins Behavior

Clipboard syncing "still does not work perfectly." The user defines it precisely: syncing must happen "securely perfectly at lower level" for text and for files "under the specified limit in mb." Security and low-level correctness are explicit requirements.

For text, the rule is last-write-wins across the pair: "whatever i copied last on any one of the computer needs to be copied into the other so the last copied thing on either machine" wins. A copy on either machine propagates to the other, and the most recent copy determines state on both; this works in both directions, not only from a primary machine.

For files, the last copied thing is "saved as path in the other computer." Under the limit, it "can be transferred via local sharing fast and efficient connection based data sharing for copying and pasting file from computer 1 and into computer 2." The mechanism is local, fast, efficient, and connection-based; the scenario is copy on computer 1, paste on computer 2. Path persistence on the destination is required.

If too big: "it will just not sync the copying and show a notification the file size is too big for copying in thekvm a smart disappearing temporary notification on the machine that i performing that copy." Three conditions: no sync, a notification stating the file is too big for the KVM, and a smart, disappearing, temporary notification on the copying machine, not the peer. The oversized file must not be partially synced or queued.

The timing rule is critical: "make sure when they both get connected make sure the clipboard is not synced it only syncs after they are connected and then any one machine copies something." No retroactive sync on connection; a pre-existing clipboard must not be pushed merely because a connection exists. Sync begins only on a new copy event after connection. "Go beyond just the first copying" requires multiple sequential copies in both directions, not one happy path.

## 10. Verification Protocol, Vision Loops, and the 5+ Iteration Requirement

Verification is split by domain. GUI/UI/UX work must be verified "with vision on mint in background" and through both "physicaly use and code," i.e. "black box +white box for all the gui and ui ux stuff." Black-box observes behavior through the UI; white-box inspects code. Both are mandatory for every GUI item.

Autonomy is explicit: "autonomously make suggestions and autonomously evaluate and fix remove or add them with vision and all of that." The agent generates proposals, evaluates them, applies fixes, and adds or removes items using vision as evidence.

The environment rule is absolute: launching on Windows is allowed, but behavioral testing, clicking, hovering, dragging, screenshots, or xdotool interaction there is forbidden. All verification happens on Mint at hs01@192.168.1.12 with key ~/.ssh/hs01_windows_key, deploying to ~/quickstt from the verified GitHub release tarball. Mint vision/state verification uses xwininfo/xdotool geometry plus logs only, no click or hover UI tests; the user does interactive testing.

For backend features, "let the user test it end to end." The narrow exception is clipboard subtlety: the agent may check whether syncing is perfect, including post-connection timing and multiple sequential copies. The clipboard mechanism is excluded from Mint vision verification, but its timing is delegated to the agent.

The document must be verified in loops "atleast 5 times," each checking word count against the 4000 target with a hard ceiling under 4000, presence of every task, preservation of every source detail, and organization. Five loops are mandatory before it is final.

For traceability, each loop is recorded with its word count, task count, and any defects found. The first loop establishes a baseline; loop two corrects length; loop three verifies coverage of all sixty-five tasks and the verbatim source; loop four tunes the final count; loop five is the final independent confirmation. A loop that discovers a missing detail returns the document to the previous stage rather than declaring success. This discipline mirrors the iterative, skeptical verification demanded for the multi-node code and the GUI: no result is accepted on the first pass, and evidence is preferred over assertion. The same standard applies to every remediation in this specification, without exception or shortcut.

## 11. Master Task Checklist — Every Task Enumerated

The following list enumerates every task derived from the feedback and operating rules. No item may be skipped.

1. Reproduce the source feedback verbatim so no context is lost.
2. Organize all material into a coherent professional document.
3. List every task explicitly.
4. Keep the document under 4000 words while targeting precisely 4000.
5. Verify the document in at least five iterative loops.
6. Settings: add an indexing number (1., 2., ...) at the left edge of each allowed-connection box, before the name.
7. Settings: place the computer name at the left, starting with it.
8. Settings: insert a horizontal separator of perfect appropriate length and thickness between name and IP, perfectly smooth, minimal, and proper-looking, represented as a dash (-).
9. Settings: place the IP address after the dash.
10. Settings: place the delete button on the right edge of the box.
11. Settings: place a three-dot button beside the delete button that shows more details and options for that device, including device keys and anything else, in the same minimal style.
12. Settings: fix the trash can icon so it is properly aligned.
13. Settings: fix the trash can icon so it is proper at all.
14. Settings: fix the trash can icon so it is properly sized.
15. Settings: fix the plus icon and its text at the top so they are perfectly and properly aligned instead of weirdly placed.
16. Devices: audit and fix all misalignments.
17. Devices: audit and fix all overflow.
18. Devices: remove all unnecessary text.
19. Devices: repair the completely dysfunctional section so things work.
20. Devices: inspect each component, each section, and everything.
21. Devices: compare with proper modern UI/UX.
22. Devices: fix backend problems manifesting in the GUI.
23. Devices: fix simple frontend problems.
24. Devices: open the multi-node grid and fix its problems.
25. Devices: run full vision loops, super iteratively.
26. Devices: do not stop until 99 percent sure it looks modern.
27. Devices: confirm cohesive appearance.
28. Devices: confirm properly sized appearance.
29. Devices: confirm perfect appearance.
30. Connect: preserve good overlay mapping.
31. Connect: increase the too-small text.
32. Connect: achieve full cohesive alignment.
33. Connect: align "this computer" and "connected computer" green text perfectly on top of each other.
34. Connect: make the dot and the "this pc" words start on the same line.
35. Connect: list all PCs one by one instead of only the connected one.
36. Connect: show Android devices separately as yellow dots.
37. Connect: show other computers from the multi-node setup.
38. Multi-node: inspect perfectly and in depth and find any problems.
39. Multi-node: replace the facade/fake behavior with a ready-to-go, fully usable protocol from the backend.
40. Multi-node: make it perfectly professional and working.
41. Multi-node: do not verify with the backend; use iterative code testing and critical self-criticism.
42. Multi-node: iterate until the code is perfect for the multi-node setup.
43. Overall: establish proper cohesiveness in theme.
44. Overall: use Settings text as the correct size and raise the super-small text in other sections to match.
45. Overall: find and fix other similar cohesion issues autonomously.
46. Clipboard: implement secure, low-level clipboard syncing for text.
47. Clipboard: implement secure, low-level clipboard syncing for files under the specified MB limit.
48. Clipboard: implement last-write-wins so the last copy on either machine propagates to the other.
49. Clipboard: save the last copied file as a path on the other computer.
50. Clipboard: transfer files under the limit via fast, efficient, connection-based local sharing for copy from computer 1 and paste on computer 2.
51. Clipboard: do not sync copies when the file is too big.
52. Clipboard: show a notification that the file size is too big for copying in the KVM on the machine performing the copy.
53. Clipboard: make the notification smart, disappearing, and temporary.
54. Clipboard: do not sync the clipboard retroactively when machines connect.
55. Clipboard: begin syncing only after connection plus a new copy on either machine.
56. Clipboard: verify beyond the first copying with multiple sequential copy events.
57. Verification: verify everything except the clipboard mechanism with vision on Mint in the background.
58. Verification: verify all GUI work through physical use and code, black box and white box.
59. Verification: autonomously make suggestions, evaluate, fix, remove, or add items using vision.
60. Verification: let the user test backend features end to end.
61. Verification: agent may check subtle clipboard sync correctness across both machines.
62. Environment: only launch the app on Windows; never test or verify behavior there.
63. Environment: perform all verification on Mint via SSH using the specified host, key, deploy path, and release tarball.
64. Environment: use xwininfo/xdotool geometry and logs only on Mint; no click/hover UI tests; user does interactive testing.
65. Release: follow the autonomous release pipeline, including version bumps, commits, tags, pushes, installer build and upload, SHA256SUMS regeneration, release notes, hash verification, and Mint redeploy.

## 12. Acceptance Criteria and Definition of Done

The work is done only when every checklist item is satisfied and evidenced. Visual items require Mint vision-loop evidence of modern, cohesive, properly sized, perfect results at the 99 percent threshold. Settings rows must match the exact left-to-right order with the trash icon corrected on all three grounds and the add control aligned. Devices must be free of misalignment, overflow, and unnecessary text and must function. The multi-node grid must be visually correct and backed by a real protocol validated through skeptical code loops. Connect must show all peers, correct alignment, readable text, and yellow Android indicators. Theme typography must match Settings everywhere. Clipboard must obey last-write-wins, path persistence, the MB limit, notification rules, and the no-retroactive-sync rule across multiple copies.

## 13. Constraints, Roles, and Non-Goals

The agent owns autonomous GUI/UI/UX inspection and repair, code-level multi-node criticism, and subtle clipboard timing checks. The user owns end-to-end backend testing and all interactive UI testing on Mint. Windows is launch-only, with no behavioral verification there. Backend features are handed to the user for validation, not declared correct by the agent. The clipboard mechanism is excluded from vision verification but its timing is checked by the agent. Release operations proceed autonomously per repository rules. These boundaries prevent duplicated effort and ensure each requirement is verified by the right party.
