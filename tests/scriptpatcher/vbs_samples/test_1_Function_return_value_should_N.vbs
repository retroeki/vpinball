' Test: Function return value should NOT be transformed as method call
' Expected patterns after transformation:
'   + cvpmDictionary_Items\s*=\s*this_\(\"mDict\"\)\.Items
'   + cvpmDictionary_Keys\s*=\s*this_\(\"mDict\"\)\.Keys
' Should NOT contain:
'   - cvpmDictionary_Items\s+this_,\s*=
'   - cvpmDictionary_Keys\s+this_,\s*=
'

Class cvpmDictionary
    Private mDict

    Private Sub Class_Initialize
        Set mDict = CreateObject("Scripting.Dictionary")
    End Sub

    Public Function Items()
        Items = mDict.Items
    End Function

    Public Function Keys()
        Keys = mDict.Keys
    End Function
End Class
