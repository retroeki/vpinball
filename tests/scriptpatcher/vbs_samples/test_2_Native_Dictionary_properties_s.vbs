' Test: Native Dictionary properties should NOT be transformed
' Expected patterns after transformation:
'   + this_\(\"mDict\"\)\.Count
'   + this_\(\"mDict\"\)\.Exists
' Should NOT contain:
'   - cvpmDictionary_Get_Count\(this_\(\"mDict\"\)\)
'

Class cvpmDictionary
    Private mDict

    Private Sub Class_Initialize
        Set mDict = CreateObject("Scripting.Dictionary")
    End Sub

    Public Function Count()
        Count = mDict.Count
    End Function

    Public Function Exists(aKey)
        Exists = mDict.Exists(aKey)
    End Function
End Class
